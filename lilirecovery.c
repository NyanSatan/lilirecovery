/*
 * libirecovery.c
 * Communication to iBoot/iBSS on Apple iOS devices via USB
 *
 * Copyright (c) 2011-2023 Nikias Bassen <nikias@gmx.li>
 * Copyright (c) 2012-2020 Martin Szulecki <martin.szulecki@libimobiledevice.org>
 * Copyright (c) 2010 Chronic-Dev Team
 * Copyright (c) 2010 Joshua Hill
 * Copyright (c) 2008-2011 Nicolas Haunold
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

#define IRECV_API __attribute__((visibility("default")))

#include "lilirecovery.h"

struct irecv_client_private {
    int debug;
    int usb_config;
    int usb_interface;
    int usb_alt_interface;
    unsigned int mode;
    int isKIS;
    struct irecv_device_info device_info;
    IOUSBDeviceInterface320 **handle;
    IOUSBInterfaceInterface300 **usbInterface;
    irecv_event_cb_t progress_callback;
    irecv_event_cb_t received_callback;
    irecv_event_cb_t connected_callback;
    irecv_event_cb_t precommand_callback;
    irecv_event_cb_t postcommand_callback;
    irecv_event_cb_t disconnected_callback;
};

#define USB_TIMEOUT 10000
#define APPLE_VENDOR_ID 0x05AC

// KIS
#define KIS_PRODUCT_ID 0x1881

#define KIS_PORTAL_CONFIG 0x01
#define KIS_PORTAL_RSM    0x10

#define KIS_INDEX_UPLOAD   0x0D
#define KIS_INDEX_ENABLE_A 0x0A // macOS writes to this
#define KIS_INDEX_ENABLE_B 0x14 // macOS writes to this
#define KIS_INDEX_GET_INFO 0x100
#define KIS_INDEX_BOOT_IMG 0x103

#define KIS_ENABLE_A_VAL   0x21 // Value to write to KIS_INDEX_ENABLE_A
#define KIS_ENABLE_B_VAL   0x01 // Value to write to KIS_INDEX_ENABLE_B

#define BUFFER_SIZE 0x1000
#define debug(...) if (libirecovery_debug) fprintf(stderr, __VA_ARGS__)

static int libirecovery_debug = 0;

static struct irecv_device irecv_devices[] = {
    /* iPhone */
    { "iPhone1,1",   "m68ap",    0x00, 0x8900, "iPhone 2G" },
    { "iPhone1,2",   "n82ap",    0x04, 0x8900, "iPhone 3G" },
    { "iPhone2,1",   "n88ap",    0x00, 0x8920, "iPhone 3Gs" },
    { "iPhone3,1",   "n90ap",    0x00, 0x8930, "iPhone 4 (GSM)" },
    { "iPhone3,2",   "n90bap",   0x04, 0x8930, "iPhone 4 (GSM) R2 2012" },
    { "iPhone3,3",   "n92ap",    0x06, 0x8930, "iPhone 4 (CDMA)" },
    { "iPhone4,1",   "n94ap",    0x08, 0x8940, "iPhone 4s" },
    { "iPhone5,1",   "n41ap",    0x00, 0x8950, "iPhone 5 (GSM)" },
    { "iPhone5,2",   "n42ap",    0x02, 0x8950, "iPhone 5 (Global)" },
    { "iPhone5,3",   "n48ap",    0x0a, 0x8950, "iPhone 5c (GSM)" },
    { "iPhone5,4",   "n49ap",    0x0e, 0x8950, "iPhone 5c (Global)" },
    { "iPhone6,1",   "n51ap",    0x00, 0x8960, "iPhone 5s (GSM)" },
    { "iPhone6,2",   "n53ap",    0x02, 0x8960, "iPhone 5s (Global)" },
    { "iPhone7,1",   "n56ap",    0x04, 0x7000, "iPhone 6 Plus" },
    { "iPhone7,2",   "n61ap",    0x06, 0x7000, "iPhone 6" },
    { "iPhone8,1",   "n71ap",    0x04, 0x8000, "iPhone 6s" },
    { "iPhone8,1",   "n71map",   0x04, 0x8003, "iPhone 6s" },
    { "iPhone8,2",   "n66ap",    0x06, 0x8000, "iPhone 6s Plus" },
    { "iPhone8,2",   "n66map",   0x06, 0x8003, "iPhone 6s Plus" },
    { "iPhone8,4",   "n69ap",    0x02, 0x8003, "iPhone SE (1st gen)" },
    { "iPhone8,4",   "n69uap",   0x02, 0x8000, "iPhone SE (1st gen)" },
    { "iPhone9,1",   "d10ap",    0x08, 0x8010, "iPhone 7 (Global)" },
    { "iPhone9,2",   "d11ap",    0x0a, 0x8010, "iPhone 7 Plus (Global)" },
    { "iPhone9,3",   "d101ap",   0x0c, 0x8010, "iPhone 7 (GSM)" },
    { "iPhone9,4",   "d111ap",   0x0e, 0x8010, "iPhone 7 Plus (GSM)" },
    { "iPhone10,1",  "d20ap",    0x02, 0x8015, "iPhone 8 (Global)" },
    { "iPhone10,2",  "d21ap",    0x04, 0x8015, "iPhone 8 Plus (Global)" },
    { "iPhone10,3",  "d22ap",    0x06, 0x8015, "iPhone X (Global)" },
    { "iPhone10,4",  "d201ap",   0x0a, 0x8015, "iPhone 8 (GSM)" },
    { "iPhone10,5",  "d211ap",   0x0c, 0x8015, "iPhone 8 Plus (GSM)" },
    { "iPhone10,6",  "d221ap",   0x0e, 0x8015, "iPhone X (GSM)" },
    { "iPhone11,2",  "d321ap",   0x0e, 0x8020, "iPhone XS" },
    { "iPhone11,4",  "d331ap",   0x0a, 0x8020, "iPhone XS Max (China)" },
    { "iPhone11,6",  "d331pap",  0x1a, 0x8020, "iPhone XS Max" },
    { "iPhone11,8",  "n841ap",   0x0c, 0x8020, "iPhone XR" },
    { "iPhone12,1",  "n104ap",   0x04, 0x8030, "iPhone 11" },
    { "iPhone12,3",  "d421ap",   0x06, 0x8030, "iPhone 11 Pro" },
    { "iPhone12,5",  "d431ap",   0x02, 0x8030, "iPhone 11 Pro Max" },
    { "iPhone12,8",  "d79ap",    0x10, 0x8030, "iPhone SE (2nd gen)" },
    { "iPhone13,1",  "d52gap",   0x0A, 0x8101, "iPhone 12 mini" },
    { "iPhone13,2",  "d53gap",   0x0C, 0x8101, "iPhone 12" },
    { "iPhone13,3",  "d53pap",   0x0E, 0x8101, "iPhone 12 Pro" },
    { "iPhone13,4",  "d54pap",   0x08, 0x8101, "iPhone 12 Pro Max" },
    { "iPhone14,2",  "d63ap",    0x0C, 0x8110, "iPhone 13 Pro" },
    { "iPhone14,3",  "d64ap",    0x0E, 0x8110, "iPhone 13 Pro Max" },
    { "iPhone14,4",  "d16ap",    0x08, 0x8110, "iPhone 13 mini" },
    { "iPhone14,5",  "d17ap",    0x0A, 0x8110, "iPhone 13" },
    { "iPhone14,6",  "d49ap",    0x10, 0x8110, "iPhone SE (3rd gen)" },
    { "iPhone14,7",     "d27ap",    0x18, 0x8110, "iPhone 14" },
    { "iPhone14,8",     "d28ap",    0x1A, 0x8110, "iPhone 14 Plus" },
    { "iPhone15,2",     "d73ap",    0x0C, 0x8120, "iPhone 14 Pro" },
    { "iPhone15,3",     "d74ap",    0x0E, 0x8120, "iPhone 14 Pro Max" },
    { "iPhone15,4",     "d37ap",    0x08, 0x8120, "iPhone 15" },
    { "iPhone15,5",     "d38ap",    0x0A, 0x8120, "iPhone 15 Plus" },
    { "iPhone16,1",     "d83ap",    0x04, 0x8130, "iPhone 15 Pro" },
    { "iPhone16,2",     "d84ap",    0x06, 0x8130, "iPhone 15 Pro Max" },
    { "iPhone17,1",     "d93ap",    0x0C, 0x8140, "iPhone 16 Pro" },
    { "iPhone17,2",     "d94ap",    0x0E, 0x8140, "iPhone 16 Pro Max" },
    { "iPhone17,3",     "d47ap",    0x08, 0x8140, "iPhone 16" },
    { "iPhone17,4",     "d48ap",    0x0A, 0x8140, "iPhone 16 Plus" },
    /* iPod */
    { "iPod1,1",     "n45ap",    0x02, 0x8900, "iPod Touch (1st gen)" },
    { "iPod2,1",     "n72ap",    0x00, 0x8720, "iPod Touch (2nd gen)" },
    { "iPod3,1",     "n18ap",    0x02, 0x8922, "iPod Touch (3rd gen)" },
    { "iPod4,1",     "n81ap",    0x08, 0x8930, "iPod Touch (4th gen)" },
    { "iPod5,1",     "n78ap",    0x00, 0x8942, "iPod Touch (5th gen)" },
    { "iPod7,1",     "n102ap",   0x10, 0x7000, "iPod Touch (6th gen)" },
    { "iPod9,1",     "n112ap",   0x16, 0x8010, "iPod Touch (7th gen)" },
    /* iPad */
    { "iPad1,1",     "k48ap",    0x02, 0x8930, "iPad" },
    { "iPad2,1",     "k93ap",    0x04, 0x8940, "iPad 2 (WiFi)" },
    { "iPad2,2",     "k94ap",    0x06, 0x8940, "iPad 2 (GSM)" },
    { "iPad2,3",     "k95ap",    0x02, 0x8940, "iPad 2 (CDMA)" },
    { "iPad2,4",     "k93aap",   0x06, 0x8942, "iPad 2 (WiFi) R2 2012" },
    { "iPad2,5",     "p105ap",   0x0a, 0x8942, "iPad mini (WiFi)" },
    { "iPad2,6",     "p106ap",   0x0c, 0x8942, "iPad mini (GSM)" },
    { "iPad2,7",     "p107ap",   0x0e, 0x8942, "iPad mini (Global)" },
    { "iPad3,1",     "j1ap",     0x00, 0x8945, "iPad (3rd gen, WiFi)" },
    { "iPad3,2",     "j2ap",     0x02, 0x8945, "iPad (3rd gen, CDMA)" },
    { "iPad3,3",     "j2aap",    0x04, 0x8945, "iPad (3rd gen, GSM)" },
    { "iPad3,4",     "p101ap",   0x00, 0x8955, "iPad (4th gen, WiFi)" },
    { "iPad3,5",     "p102ap",   0x02, 0x8955, "iPad (4th gen, GSM)" },
    { "iPad3,6",     "p103ap",   0x04, 0x8955, "iPad (4th gen, Global)" },
    { "iPad4,1",     "j71ap",    0x10, 0x8960, "iPad Air (WiFi)" },
    { "iPad4,2",     "j72ap",    0x12, 0x8960, "iPad Air (Cellular)" },
    { "iPad4,3",     "j73ap",    0x14, 0x8960, "iPad Air (China)" },
    { "iPad4,4",     "j85ap",    0x0a, 0x8960, "iPad mini 2 (WiFi)" },
    { "iPad4,5",     "j86ap",    0x0c, 0x8960, "iPad mini 2 (Cellular)" },
    { "iPad4,6",     "j87ap",    0x0e, 0x8960, "iPad mini 2 (China)" },
    { "iPad4,7",     "j85map",   0x32, 0x8960, "iPad mini 3 (WiFi)" },
    { "iPad4,8",     "j86map",   0x34, 0x8960, "iPad mini 3 (Cellular)" },
    { "iPad4,9",     "j87map",   0x36, 0x8960, "iPad mini 3 (China)" },
    { "iPad5,1",     "j96ap",    0x08, 0x7000, "iPad mini 4 (WiFi)" },
    { "iPad5,2",     "j97ap",    0x0A, 0x7000, "iPad mini 4 (Cellular)" },
    { "iPad5,3",     "j81ap",    0x06, 0x7001, "iPad Air 2 (WiFi)" },
    { "iPad5,4",     "j82ap",    0x02, 0x7001, "iPad Air 2 (Cellular)" },
    { "iPad6,3",     "j127ap",   0x08, 0x8001, "iPad Pro 9.7-inch (WiFi)" },
    { "iPad6,4",     "j128ap",   0x0a, 0x8001, "iPad Pro 9.7-inch (Cellular)" },
    { "iPad6,7",     "j98aap",   0x10, 0x8001, "iPad Pro 12.9-inch (1st gen, WiFi)" },
    { "iPad6,8",     "j99aap",   0x12, 0x8001, "iPad Pro 12.9-inch (1st gen, Cellular)" },
    { "iPad6,11",    "j71sap",   0x10, 0x8000, "iPad (5th gen, WiFi)" },
    { "iPad6,11",    "j71tap",   0x10, 0x8003, "iPad (5th gen, WiFi)" },
    { "iPad6,12",    "j72sap",   0x12, 0x8000, "iPad (5th gen, Cellular)" },
    { "iPad6,12",    "j72tap",   0x12, 0x8003, "iPad (5th gen, Cellular)" },
    { "iPad7,1",     "j120ap",   0x0C, 0x8011, "iPad Pro 12.9-inch (2nd gen, WiFi)" },
    { "iPad7,2",     "j121ap",   0x0E, 0x8011, "iPad Pro 12.9-inch (2nd gen, Cellular)" },
    { "iPad7,3",     "j207ap",   0x04, 0x8011, "iPad Pro 10.5-inch (WiFi)" },
    { "iPad7,4",     "j208ap",   0x06, 0x8011, "iPad Pro 10.5-inch (Cellular)" },
    { "iPad7,5",     "j71bap",   0x18, 0x8010, "iPad (6th gen, WiFi)" },
    { "iPad7,6",     "j72bap",   0x1A, 0x8010, "iPad (6th gen, Cellular)" },
    { "iPad7,11",    "j171ap",   0x1C, 0x8010, "iPad (7th gen, WiFi)" },
    { "iPad7,12",    "j172ap",   0x1E, 0x8010, "iPad (7th gen, Cellular)" },
    { "iPad8,1",     "j317ap",   0x0C, 0x8027, "iPad Pro 11-inch (1st gen, WiFi)" },
    { "iPad8,2",     "j317xap",  0x1C, 0x8027, "iPad Pro 11-inch (1st gen, WiFi, 1TB)" },
    { "iPad8,3",     "j318ap",   0x0E, 0x8027, "iPad Pro 11-inch (1st gen, Cellular)" },
    { "iPad8,4",     "j318xap",  0x1E, 0x8027, "iPad Pro 11-inch (1st gen, Cellular, 1TB)" },
    { "iPad8,5",     "j320ap",   0x08, 0x8027, "iPad Pro 12.9-inch (3rd gen, WiFi)" },
    { "iPad8,6",     "j320xap",  0x18, 0x8027, "iPad Pro 12.9-inch (3rd gen, WiFi, 1TB)" },
    { "iPad8,7",     "j321ap",   0x0A, 0x8027, "iPad Pro 12.9-inch (3rd gen, Cellular)" },
    { "iPad8,8",     "j321xap",  0x1A, 0x8027, "iPad Pro 12.9-inch (3rd gen, Cellular, 1TB)" },
    { "iPad8,9",     "j417ap",   0x3C, 0x8027, "iPad Pro 11-inch (2nd gen, WiFi)" },
    { "iPad8,10",    "j418ap",   0x3E, 0x8027, "iPad Pro 11-inch (2nd gen, Cellular)" },
    { "iPad8,11",    "j420ap",   0x38, 0x8027, "iPad Pro 12.9-inch (4th gen, WiFi)" },
    { "iPad8,12",    "j421ap",   0x3A, 0x8027, "iPad Pro 12.9-inch (4th gen, Cellular)" },
    { "iPad11,1",    "j210ap",   0x14, 0x8020, "iPad mini (5th gen, WiFi)" },
    { "iPad11,2",    "j211ap",   0x16, 0x8020, "iPad mini (5th gen, Cellular)" },
    { "iPad11,3",    "j217ap",   0x1C, 0x8020, "iPad Air (3rd gen, WiFi)" },
    { "iPad11,4",    "j218ap",   0x1E, 0x8020, "iPad Air (3rd gen, Cellular)" },
    { "iPad11,6",    "j171aap",  0x24, 0x8020, "iPad (8th gen, WiFi)" },
    { "iPad11,7",    "j172aap",  0x26, 0x8020, "iPad (8th gen, Cellular)" },
    { "iPad12,1",    "j181ap",   0x18, 0x8030, "iPad (9th gen, WiFi)" },
    { "iPad12,2",    "j182ap",   0x1A, 0x8030, "iPad (9th gen, Cellular)" },
    { "iPad13,1",    "j307ap",   0x04, 0x8101, "iPad Air (4th gen, WiFi)" },
    { "iPad13,2",    "j308ap",   0x06, 0x8101, "iPad Air (4th gen, Cellular)" },
    { "iPad13,4",    "j517ap",   0x08, 0x8103, "iPad Pro 11-inch (3rd gen, WiFi)" },
    { "iPad13,5",    "j517xap",  0x0A, 0x8103, "iPad Pro 11-inch (3rd gen, WiFi, 2TB)" },
    { "iPad13,6",    "j518ap",   0x0C, 0x8103, "iPad Pro 11-inch (3rd gen, Cellular)" },
    { "iPad13,7",    "j518xap",  0x0E, 0x8103, "iPad Pro 11-inch (3rd gen, Cellular, 2TB)" },
    { "iPad13,8",    "j522ap",   0x18, 0x8103, "iPad Pro 12.9-inch (5th gen, WiFi)" },
    { "iPad13,9",    "j522xap",  0x1A, 0x8103, "iPad Pro 12.9-inch (5th gen, WiFi, 2TB)" },
    { "iPad13,10",   "j523ap",   0x1C, 0x8103, "iPad Pro 12.9-inch (5th gen, Cellular)" },
    { "iPad13,11",   "j523xap",  0x1E, 0x8103, "iPad Pro 12.9-inch (5th gen, Cellular, 2TB)" },
    { "iPad13,16",   "j407ap",   0x10, 0x8103, "iPad Air (5th gen, WiFi)" },
    { "iPad13,17",   "j408ap",   0x12, 0x8103, "iPad Air (5th gen, Cellular)" },
    { "iPad13,18",   "j271ap",   0x14, 0x8101, "iPad (10th gen, WiFi)" },
    { "iPad13,19",   "j272ap",   0x16, 0x8101, "iPad (10th gen, Cellular)" },
    { "iPad14,1",    "j310ap",   0x04, 0x8110, "iPad mini (6th gen, WiFi)" },
    { "iPad14,2",    "j311ap",   0x06, 0x8110, "iPad mini (6th gen, Cellular)" },
    { "iPad14,3",    "j617ap",   0x08, 0x8112, "iPad Pro 11-inch (4th gen, WiFi)" },
    { "iPad14,4",    "j618ap",   0x0A, 0x8112, "iPad Pro 11-inch (4th gen, Cellular)" },
    { "iPad14,5",    "j620ap",   0x0C, 0x8112, "iPad Pro 12.9-inch (6th gen, WiFi)" },
    { "iPad14,6",    "j621ap",   0x0E, 0x8112, "iPad Pro 12.9-inch (6th gen, Cellular)" },
    { "iPad14,8",    "j507ap",   0x10, 0x8112, "iPad Air 11-inch (M2, WiFi)" },
    { "iPad14,9",    "j508ap",   0x12, 0x8112, "iPad Air 11-inch (M2, Cellular)" },
    { "iPad14,10",   "j537ap",   0x14, 0x8112, "iPad Air 13-inch (M2, WiFi)" },
    { "iPad14,11",   "j538ap",   0x16, 0x8112, "iPad Air 13-inch (M2, Cellular)" },
    { "iPad16,1",    "j410ap",   0x08, 0x8130, "iPad mini (A17 Pro, WiFi)" },
    { "iPad16,2",    "j411ap",   0x0A, 0x8130, "iPad mini (A17 Pro, Cellular)" },
    { "iPad16,3",    "j717ap",   0x08, 0x8132, "iPad Pro 11-inch (M4, WiFi)" },
    { "iPad16,4",    "j718ap",   0x0A, 0x8132, "iPad Pro 11-inch (M4, Cellular)" },
    { "iPad16,5",    "j720ap",   0x0C, 0x8132, "iPad Pro 13-inch (M4, WiFi)" },
    { "iPad16,6",    "j721ap",   0x0E, 0x8132, "iPad Pro 13-inch (M4, Cellular)" },
    /* Apple TV */
    { "AppleTV2,1",  "k66ap",    0x10, 0x8930, "Apple TV 2" },
    { "AppleTV3,1",  "j33ap",    0x08, 0x8942, "Apple TV 3" },
    { "AppleTV3,2",  "j33iap",   0x00, 0x8947, "Apple TV 3 (2013)" },
    { "AppleTV5,3",  "j42dap",   0x34, 0x7000, "Apple TV 4" },
    { "AppleTV6,2",  "j105aap",  0x02, 0x8011, "Apple TV 4K" },
    { "AppleTV11,1", "j305ap",   0x08, 0x8020, "Apple TV 4K (2nd gen)" },
    { "AppleTV14,1", "j255ap",   0x02, 0x8110, "Apple TV 4K (3rd gen)" },
    /* HomePod */
    { "AudioAccessory1,1",  "b238aap",  0x38, 0x7000, "HomePod (1st gen)" },
    { "AudioAccessory1,2",  "b238ap",   0x1A, 0x7000, "HomePod (1st gen)" },
    { "AudioAccessory5,1",  "b520ap",   0x22, 0x8006, "HomePod mini" },
    { "AudioAccessory6,1",  "b620ap",   0x18, 0x8301, "HomePod (2nd gen)" },
    /* Apple Watch */
    { "Watch1,1",    "n27aap",   0x02, 0x7002, "Apple Watch 38mm (1st gen)" },
    { "Watch1,2",    "n28aap",   0x04, 0x7002, "Apple Watch 42mm (1st gen)" },
    { "Watch2,6",    "n27dap",  0x02, 0x8002, "Apple Watch Series 1 (38mm)" },
    { "Watch2,7",    "n28dap",  0x04, 0x8002, "Apple Watch Series 1 (42mm)" },
    { "Watch2,3",    "n74ap",   0x0C, 0x8002, "Apple Watch Series 2 (38mm)" },
    { "Watch2,4",    "n75ap",   0x0E, 0x8002, "Apple Watch Series 2 (42mm)" },
    { "Watch3,1",    "n111sap", 0x1C, 0x8004, "Apple Watch Series 3 (38mm Cellular)" },
    { "Watch3,2",    "n111bap", 0x1E, 0x8004, "Apple Watch Series 3 (42mm Cellular)" },
    { "Watch3,3",    "n121sap", 0x18, 0x8004, "Apple Watch Series 3 (38mm)" },
    { "Watch3,4",    "n121bap", 0x1A, 0x8004, "Apple Watch Series 3 (42mm)" },
    { "Watch4,1",    "n131sap", 0x08, 0x8006, "Apple Watch Series 4 (40mm)" },
    { "Watch4,2",    "n131bap", 0x0A, 0x8006, "Apple Watch Series 4 (44mm)" },
    { "Watch4,3",    "n141sap", 0x0C, 0x8006, "Apple Watch Series 4 (40mm Cellular)" },
    { "Watch4,4",    "n141bap", 0x0E, 0x8006, "Apple Watch Series 4 (44mm Cellular)" },
    { "Watch5,1",    "n144sap", 0x10, 0x8006, "Apple Watch Series 5 (40mm)" },
    { "Watch5,2",    "n144bap", 0x12, 0x8006, "Apple Watch Series 5 (44mm)" },
    { "Watch5,3",    "n146sap", 0x14, 0x8006, "Apple Watch Series 5 (40mm Cellular)" },
    { "Watch5,4",    "n146bap", 0x16, 0x8006, "Apple Watch Series 5 (44mm Cellular)" },
    { "Watch5,9",    "n140sap", 0x28, 0x8006, "Apple Watch SE (40mm)" },
    { "Watch5,10",   "n140bap", 0x2A, 0x8006, "Apple Watch SE (44mm)" },
    { "Watch5,11",   "n142sap", 0x2C, 0x8006, "Apple Watch SE (40mm Cellular)" },
    { "Watch5,12",   "n142bap", 0x2E, 0x8006, "Apple Watch SE (44mm Cellular)" },
    { "Watch6,1",    "n157sap", 0x08, 0x8301, "Apple Watch Series 6 (40mm)" },
    { "Watch6,2",    "n157bap", 0x0A, 0x8301, "Apple Watch Series 6 (44mm)" },
    { "Watch6,3",    "n158sap", 0x0C, 0x8301, "Apple Watch Series 6 (40mm Cellular)" },
    { "Watch6,4",    "n158bap", 0x0E, 0x8301, "Apple Watch Series 6 (44mm Cellular)" },
    { "Watch6,6",    "n187sap", 0x10, 0x8301, "Apple Watch Series 7 (41mm)" },
    { "Watch6,7",    "n187bap", 0x12, 0x8301, "Apple Watch Series 7 (45mm)" },
    { "Watch6,8",    "n188sap", 0x14, 0x8301, "Apple Watch Series 7 (41mm Cellular)" },
    { "Watch6,9",    "n188bap", 0x16, 0x8301, "Apple Watch Series 7 (45mm Cellular)" },
    { "Watch6,10",   "n143sap", 0x28, 0x8301, "Apple Watch SE 2 (40mm)" },
    { "Watch6,11",   "n143bap", 0x2A, 0x8301, "Apple Watch SE 2 (44mm)" },
    { "Watch6,12",   "n149sap", 0x2C, 0x8301, "Apple Watch SE 2 (40mm Cellular)" },
    { "Watch6,13",   "n149bap", 0x2E, 0x8301, "Apple Watch SE 2 (44mm Cellular)" },
    { "Watch6,14",   "n197sap", 0x30, 0x8301, "Apple Watch Series 8 (41mm)" },
    { "Watch6,15",   "n197bap", 0x32, 0x8301, "Apple Watch Series 8 (45mm)" },
    { "Watch6,16",   "n198sap", 0x34, 0x8301, "Apple Watch Series 8 (41mm Cellular)" },
    { "Watch6,17",   "n198bap", 0x36, 0x8301, "Apple Watch Series 8 (45mm Cellular)" },
    { "Watch6,18",   "n199ap",  0x26, 0x8301, "Apple Watch Ultra" },
    { "Watch7,1",    "n207sap", 0x08, 0x8310, "Apple Watch Series 9 (41mm)" },
    { "Watch7,2",    "n207bap", 0x0A, 0x8310, "Apple Watch Series 9 (45mm)" },
    { "Watch7,3",    "n208sap", 0x0C, 0x8310, "Apple Watch Series 9 (41mm Cellular)" },
    { "Watch7,4",    "n208bap", 0x0E, 0x8310, "Apple Watch Series 9 (45mm Cellular)" },
    { "Watch7,5",    "n210ap",  0x02, 0x8310, "Apple Watch Ultra 2" },
    { "Watch7,8",    "n217sap", 0x10, 0x8310, "Apple Watch Series 10 (42mm)" },
    { "Watch7,9",    "n217bap", 0x12, 0x8310, "Apple Watch Series 10 (46mm)" },
    { "Watch7,10",   "n218sap", 0x14, 0x8310, "Apple Watch Series 10 (42mm Cellular)" },
    { "Watch7,11",   "n218bap", 0x16, 0x8310, "Apple Watch Series 10 (46mm Cellular)" },
    /* Apple Silicon Macs */
    { "ADP3,2",         "j273aap", 0x42, 0x8027, "Developer Transition Kit (2020)" },
    { "Macmini9,1",        "j274ap",  0x22, 0x8103, "Mac mini (M1, 2020)" },
    { "MacBookPro17,1", "j293ap",  0x24, 0x8103, "MacBook Pro (M1, 13-inch, 2020)" },
    { "MacBookPro18,1", "j316sap", 0x0A, 0x6000, "MacBook Pro (M1 Pro, 16-inch, 2021)" },
    { "MacBookPro18,2", "j316cap", 0x0A, 0x6001, "MacBook Pro (M1 Max, 16-inch, 2021)" },
    { "MacBookPro18,3", "j314sap", 0x08, 0x6000, "MacBook Pro (M1 Pro, 14-inch, 2021)" },
    { "MacBookPro18,4", "j314cap", 0x08, 0x6001, "MacBook Pro (M1 Max, 14-inch, 2021)" },
    { "MacBookAir10,1", "j313ap",  0x26, 0x8103, "MacBook Air (M1, 2020)" },
    { "iMac21,1",       "j456ap",  0x28, 0x8103, "iMac 24-inch (M1, Two Ports, 2021)" },
    { "iMac21,2",       "j457ap",  0x2A, 0x8103, "iMac 24-inch (M1, Four Ports, 2021)" },
    { "Mac13,1",        "j375cap", 0x04, 0x6001, "Mac Studio (M1 Max, 2022)" },
    { "Mac13,2",        "j375dap", 0x0C, 0x6002, "Mac Studio (M1 Ultra, 2022)" },
    { "Mac14,2",        "j413ap",  0x28, 0x8112, "MacBook Air (M2, 2022)" },
    { "Mac14,7",        "j493ap",  0x2A, 0x8112, "MacBook Pro (M2, 13-inch, 2022)" },
    { "Mac14,3",        "j473ap",  0x24, 0x8112, "Mac mini (M2, 2023)" },
    { "Mac14,5",        "j414cap", 0x04, 0x6021, "MacBook Pro (14-inch, M2 Max, 2023)" },
    { "Mac14,6",        "j416cap", 0x06, 0x6021, "MacBook Pro (16-inch, M2 Max, 2023)" },
    { "Mac14,8",        "j180dap", 0x08, 0x6022, "Mac Pro (2023)" },
    { "Mac14,9",        "j414sap", 0x04, 0x6020, "MacBook Pro (14-inch, M2 Pro, 2023)" },
    { "Mac14,10",       "j416sap", 0x06, 0x6020, "MacBook Pro (16-inch, M2 Pro, 2023)" },
    { "Mac14,12",       "j474sap", 0x02, 0x6020, "Mac mini (M2 Pro, 2023)" },
    { "Mac14,13",       "j475cap", 0x0A, 0x6021, "Mac Studio (M2 Max, 2023)" },
    { "Mac14,14",       "j475dap", 0x0A, 0x6022, "Mac Studio (M2 Ultra, 2023)" },
    { "Mac14,15",       "j415ap",  0x2E, 0x8112, "MacBook Air (M2, 15-inch, 2023)" },
    { "Mac15,3",        "j504ap",  0x22, 0x8122, "MacBook Pro (14-inch, M3, Nov 2023)" },
    { "Mac15,4",        "j433ap",  0x28, 0x8122, "iMac 24-inch (M3, Two Ports, 2023)" },
    { "Mac15,5",        "j434ap",  0x2A, 0x8122, "iMac 24-inch (M3, Four Ports, 2023)" },
    { "Mac15,6",        "j514sap", 0x04, 0x6030, "MacBook Pro (14-inch, M3 Pro, Nov 2023)" },
    { "Mac15,7",        "j516sap", 0x06, 0x6030, "MacBook Pro (16-inch, M3 Pro, Nov 2023)" },
    { "Mac15,8",        "j514cap", 0x44, 0x6031, "MacBook Pro (14-inch, M3 Max, Nov 2023)" },
    { "Mac15,9",        "j516cap", 0x46, 0x6031, "MacBook Pro (16-inch, M3 Max, Nov 2023)" },
    { "Mac15,10",       "j514map", 0x44, 0x6034, "MacBook Pro (14-inch, M3 Max, Nov 2023)" },
    { "Mac15,11",       "j516map", 0x46, 0x6034, "MacBook Pro (16-inch, M3 Max, Nov 2023)" },
    { "Mac15,12",       "j613ap",  0x30, 0x8122, "MacBook Air (13-inch, M3, 2024)" },
    { "Mac15,13",       "j615ap",  0x32, 0x8122, "MacBook Air (15-inch, M3, 2024)" },
    { "Mac16,1",        "j604ap",  0x22, 0x8132, "MacBook Pro (14-inch, M4, Nov 2024)" },
    { "Mac16,2",        "j623ap",  0x24, 0x8132, "iMac 24-inch (M4, Two Ports, 2024)" },
    { "Mac16,3",        "j624ap",  0x26, 0x8132, "iMac 24-inch (M4, Four Ports, 2024)" },
    { "Mac16,5",        "j616cap", 0x06, 0x6041, "MacBook Pro (16-inch, M4 Max, Nov 2024)" },
    { "Mac16,6",        "j614cap", 0x04, 0x6041, "MacBook Pro (14-inch, M4 Max, Nov 2024)" },
    { "Mac16,7",        "j616sap", 0x06, 0x6040, "MacBook Pro (16-inch, M4 Pro, Nov 2024)" },
    { "Mac16,8",        "j614sap", 0x04, 0x6040, "MacBook Pro (14-inch, M4 Pro, Nov 2024)" },
    { "Mac16,10",       "j773gap", 0x2A, 0x8132, "Mac mini (M4, 2024)" },
    { "Mac16,11",       "j773sap", 0x02, 0x6040, "Mac mini (M4 Pro, 2024)" },
    /* Apple Silicon VMs (supported by Virtualization.framework on macOS 12) */
    { "VirtualMac2,1",  "vma2macosap",  0x20, 0xFE00, "Apple Virtual Machine 1" },
    /* Apple T2 Coprocessor */
    { "iBridge2,1",     "j137ap",   0x0A, 0x8012, "Apple T2 iMacPro1,1 (j137)" },
    { "iBridge2,3",     "j680ap",   0x0B, 0x8012, "Apple T2 MacBookPro15,1 (j680)" },
    { "iBridge2,4",     "j132ap",   0x0C, 0x8012, "Apple T2 MacBookPro15,2 (j132)" },
    { "iBridge2,5",     "j174ap",   0x0E, 0x8012, "Apple T2 Macmini8,1 (j174)" },
    { "iBridge2,6",     "j160ap",   0x0F, 0x8012, "Apple T2 MacPro7,1 (j160)" },
    { "iBridge2,7",     "j780ap",   0x07, 0x8012, "Apple T2 MacBookPro15,3 (j780)" },
    { "iBridge2,8",     "j140kap",  0x17, 0x8012, "Apple T2 MacBookAir8,1 (j140k)" },
    { "iBridge2,10", "j213ap",   0x18, 0x8012, "Apple T2 MacBookPro15,4 (j213)" },
    { "iBridge2,12", "j140aap",  0x37, 0x8012, "Apple T2 MacBookAir8,2 (j140a)" },
    { "iBridge2,14", "j152fap",  0x3A, 0x8012, "Apple T2 MacBookPro16,1 (j152f)" },
    { "iBridge2,15", "j230kap",  0x3F, 0x8012, "Apple T2 MacBookAir9,1 (j230k)" },
    { "iBridge2,16", "j214kap",  0x3E, 0x8012, "Apple T2 MacBookPro16,2 (j214k)" },
    { "iBridge2,19", "j185ap",   0x22, 0x8012, "Apple T2 iMac20,1 (j185)" },
    { "iBridge2,20", "j185fap",  0x23, 0x8012, "Apple T2 iMac20,2 (j185f)" },
    { "iBridge2,21", "j223ap",   0x3B, 0x8012, "Apple T2 MacBookPro16,3 (j223)" },
    { "iBridge2,22", "j215ap",   0x38, 0x8012, "Apple T2 MacBookPro16,4 (j215)" },
    /* Apple Displays */
    { "AppleDisplay2,1", "j327ap", 0x22, 0x8030, "Studio Display" },
    /* Apple Vision Pro */
    { "RealityDevice14,1", "n301ap", 0x42, 0x8112, "Apple Vision Pro" },
    { NULL,          NULL,         -1,     -1, NULL }
};

static unsigned int crc32_lookup_t1[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
    0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
    0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
    0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
    0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
    0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
    0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
    0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
    0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
    0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
    0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
    0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
    0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
    0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
    0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
    0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
    0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
    0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
    0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
    0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
    0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
    0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D,
};

#define crc32_step(a,b) \
    a = (crc32_lookup_t1[(a & 0xFF) ^ ((unsigned char)b)] ^ (a >> 8))

__attribute__((__constructor__)) static void _irecv_init(void)
{
	char* dbglvl = getenv("LIBIRECOVERY_DEBUG_LEVEL");
	if (dbglvl) {
		libirecovery_debug = strtol(dbglvl, NULL, 0);
		irecv_set_debug_level(libirecovery_debug);
	}
}

#pragma pack(1)
typedef struct {
    uint16_t sequence;         // A sequence number
    uint8_t  version;          // Protocol version
    uint8_t  portal;           // The "portal" to connect to
    uint8_t  argCount;         // Number of arguments
    uint8_t  indexLo;          // An index
    uint8_t  indexHiRplSizeLo; // High 2 bits of index + low 6 bytes of reply size
    uint8_t  rplSizeHi;        // Reply size high bits, number of words the device should send
    uint32_t reqSize;          // Size of the complete request, including the arguments and payload, excluding the header
    // Followed by arguments and payload data
} KIS_req_header;

typedef struct {
    KIS_req_header hdr;
    uint32_t value;
} KIS_config_wr32;

typedef struct {
  uint8_t  bLength            ; ///< Size of this descriptor in bytes.
  uint8_t  bDescriptorType    ; ///< DEVICE Descriptor Type.
  uint16_t bcdUSB             ; ///< BUSB Specification Release Number in Binary-Coded Decimal (i.e., 2.10 is 210H). This field identifies the release of the USB Specification with which the device and its descriptors are compliant.

  uint8_t  bDeviceClass       ; ///< Class code (assigned by the USB-IF). \li If this field is reset to zero, each interface within a configuration specifies its own class information and the various interfaces operate independently. \li If this field is set to a value between 1 and FEH, the device supports different class specifications on different interfaces and the interfaces may not operate independently. This value identifies the class definition used for the aggregate interfaces. \li If this field is set to FFH, the device class is vendor-specific.
  uint8_t  bDeviceSubClass    ; ///< Subclass code (assigned by the USB-IF). These codes are qualified by the value of the bDeviceClass field. \li If the bDeviceClass field is reset to zero, this field must also be reset to zero. \li If the bDeviceClass field is not set to FFH, all values are reserved for assignment by the USB-IF.
  uint8_t  bDeviceProtocol    ; ///< Protocol code (assigned by the USB-IF). These codes are qualified by the value of the bDeviceClass and the bDeviceSubClass fields. If a device supports class-specific protocols on a device basis as opposed to an interface basis, this code identifies the protocols that the device uses as defined by the specification of the device class. \li If this field is reset to zero, the device does not use class-specific protocols on a device basis. However, it may use classspecific protocols on an interface basis. \li If this field is set to FFH, the device uses a vendor-specific protocol on a device basis.
  uint8_t  bMaxPacketSize0    ; ///< Maximum packet size for endpoint zero (only 8, 16, 32, or 64 are valid). For HS devices is fixed to 64.

  uint16_t idVendor           ; ///< Vendor ID (assigned by the USB-IF).
  uint16_t idProduct          ; ///< Product ID (assigned by the manufacturer).
  uint16_t bcdDevice          ; ///< Device release number in binary-coded decimal.
  uint8_t  iManufacturer      ; ///< Index of string descriptor describing manufacturer.
  uint8_t  iProduct           ; ///< Index of string descriptor describing product.
  uint8_t  iSerialNumber      ; ///< Index of string descriptor describing the device's serial number.

  uint8_t  bNumConfigurations ; ///< Number of possible configurations.
} usb_device_descriptor;

typedef struct {
    KIS_req_header hdr;
    union {
        struct {
            uint32_t              tag;
            uint32_t              unk1;
            uint32_t              maxUploadSize;
            uint32_t              maxDownloadSize; // maybe???
            uint64_t              rambase;
            uint32_t              nonceOffset;
            uint32_t              pad;
            uint8_t               unkpad[0x20];
            usb_device_descriptor deviceDescriptor;
        };
        uint8_t deviceInfo[0x300];
    };
    uint32_t rspsize;
    uint32_t statuscode;
} KIS_device_info;

typedef struct {
    KIS_req_header hdr;
    uint64_t address;
    uint32_t size;
    uint8_t data[0x4000];
} KIS_upload_chunk;

typedef struct {
    KIS_req_header hdr;
    uint32_t size; // Number of bytes read/written
    uint32_t status;
} KIS_generic_reply;
#pragma pack()

static int iokit_get_string_descriptor_ascii(irecv_client_t client, uint8_t desc_index, unsigned char * buffer, int size)
{
    IOReturn result;
    IOUSBDevRequest request;
    unsigned char descriptor[256];

    request.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
    request.bRequest = kUSBRqGetDescriptor;
    request.wValue = (kUSBStringDesc << 8); // | desc_index;
    request.wIndex = 0; // All languages 0x409; // language
    request.wLength = sizeof(descriptor) - 1;
    request.pData = descriptor;
    request.wLenDone = 0;

    result = (*client->handle)->DeviceRequest(client->handle, &request);
    if (result == kIOReturnNoDevice)
        return IRECV_E_NO_DEVICE;
    if (result == kIOReturnNotOpen)
        return IRECV_E_USB_STATUS;
    if (result != kIOReturnSuccess)
        return IRECV_E_UNKNOWN_ERROR;

    if (descriptor[0] >= 4) { // && descriptor[2] == 0x9 && descriptor[3] == 0x4) {

        request.wValue = (kUSBStringDesc << 8) | desc_index;
        request.wIndex = descriptor[2] + (descriptor[3] << 8);
        request.wLenDone = 0;
        result = (*client->handle)->DeviceRequest(client->handle, &request);

        if (result == kIOReturnNoDevice)
            return IRECV_E_NO_DEVICE;
        if (result == kIOReturnNotOpen)
            return IRECV_E_USB_STATUS;
        if (result != kIOReturnSuccess)
            return IRECV_E_UNKNOWN_ERROR;

        int i = 2, j = 0;
        for ( ; i < descriptor[0]; i += 2, j += 1) {
            buffer[j] = descriptor[i];
        }
        buffer[j] = 0;

        return request.wLenDone;
    }
    return IRECV_E_UNKNOWN_ERROR;
}

static int irecv_get_string_descriptor_ascii(irecv_client_t client, uint8_t desc_index, unsigned char * buffer, int size)
{
    return iokit_get_string_descriptor_ascii(client, desc_index, buffer, size);
}

static void irecv_load_device_info_from_iboot_string(irecv_client_t client, const char* iboot_string)
{
    if (!client || !iboot_string) {
        return;
    }

    memset(&client->device_info, '\0', sizeof(struct irecv_device_info));

    client->device_info.serial_string = strdup(iboot_string);

    char* ptr;

    ptr = strstr(iboot_string, "CPID:");
    if (ptr != NULL) {
        sscanf(ptr, "CPID:%x", &client->device_info.cpid);
    }

    ptr = strstr(iboot_string, "CPRV:");
    if (ptr != NULL) {
        sscanf(ptr, "CPRV:%x", &client->device_info.cprv);
    }

    ptr = strstr(iboot_string, "CPFM:");
    if (ptr != NULL) {
        sscanf(ptr, "CPFM:%x", &client->device_info.cpfm);
    }

    ptr = strstr(iboot_string, "SCEP:");
    if (ptr != NULL) {
        sscanf(ptr, "SCEP:%x", &client->device_info.scep);
    }

    ptr = strstr(iboot_string, "BDID:");
    if (ptr != NULL) {
        uint64_t bdid = 0;
        sscanf(ptr, "BDID:%" SCNx64, &bdid);
        client->device_info.bdid = (unsigned int)bdid;
    }

    ptr = strstr(iboot_string, "ECID:");
    if (ptr != NULL) {
        sscanf(ptr, "ECID:%" SCNx64, &client->device_info.ecid);
    }

    ptr = strstr(iboot_string, "IBFL:");
    if (ptr != NULL) {
        sscanf(ptr, "IBFL:%x", &client->device_info.ibfl);
    }

    char tmp[256];
    tmp[0] = '\0';
    ptr = strstr(iboot_string, "SRNM:[");
    if (ptr != NULL) {
        sscanf(ptr, "SRNM:[%s]", tmp);
        ptr = strrchr(tmp, ']');
        if (ptr != NULL) {
            *ptr = '\0';
        }
        client->device_info.srnm = strdup(tmp);
    }

    tmp[0] = '\0';
    ptr = strstr(iboot_string, "IMEI:[");
    if (ptr != NULL) {
        sscanf(ptr, "IMEI:[%s]", tmp);
        ptr = strrchr(tmp, ']');
        if (ptr != NULL) {
            *ptr = '\0';
        }
        client->device_info.imei = strdup(tmp);
    }

    tmp[0] = '\0';
    ptr = strstr(iboot_string, "SRTG:[");
    if (ptr != NULL) {
        sscanf(ptr, "SRTG:[%s]", tmp);
        ptr = strrchr(tmp, ']');
        if (ptr != NULL) {
            *ptr = '\0';
        }
        client->device_info.srtg = strdup(tmp);
    }

    client->device_info.pid = client->mode;
    if (client->isKIS) {
        client->device_info.pid = KIS_PRODUCT_ID;
    }
}

static void irecv_copy_nonce_with_tag_from_buffer(const char* tag, unsigned char** nonce, unsigned int* nonce_size, const char *buf)
{
    int taglen = strlen(tag);
    int nlen = 0;
    const char* nonce_string = NULL;
    const char* p = buf;
    char* colon = NULL;
    do {
        colon = strchr(p, ':');
        if (!colon)
            break;
        if (colon-taglen < p) {
            break;
        }
        char *space = strchr(colon, ' ');
        if (strncmp(colon-taglen, tag, taglen) == 0) {
            p = colon+1;
            if (!space) {
                nlen = strlen(p);
            } else {
                nlen = space-p;
            }
            nonce_string = p;
            nlen/=2;
            break;
        } else {
            if (!space) {
                break;
            } else {
                p = space+1;
            }
        }
    } while (colon);

    if (nlen == 0) {
        debug("%s: WARNING: couldn't find tag %s in string %s\n", __func__, tag, buf);
        return;
    }

    unsigned char *nn = malloc(nlen);
    if (!nn) {
        return;
    }

    int i = 0;
    for (i = 0; i < nlen; i++) {
        int val = 0;
        if (sscanf(nonce_string+(i*2), "%02X", &val) == 1) {
            nn[i] = (unsigned char)val;
        } else {
            debug("%s: ERROR: unexpected data in nonce result (%2s)\n", __func__, nonce_string+(i*2));
            break;
        }
    }

    if (i != nlen) {
        debug("%s: ERROR: unable to parse nonce\n", __func__);
        free(nn);
        return;
    }

    *nonce = nn;
    *nonce_size = nlen;
}

static void irecv_copy_nonce_with_tag(irecv_client_t client, const char* tag, unsigned char** nonce, unsigned int* nonce_size)
{
    if (!client || !tag) {
        return;
    }

    char buf[256];
    int len = 0;

    *nonce = NULL;
    *nonce_size = 0;

    memset(buf, 0, 256);
    len = irecv_get_string_descriptor_ascii(client, 1, (unsigned char*) buf, 255);
    if (len < 0) {
        debug("%s: got length: %d\n", __func__, len);
        return;
    }

    buf[len] = 0;

    irecv_copy_nonce_with_tag_from_buffer(tag,nonce,nonce_size,buf);
}

static irecv_error_t irecv_kis_request_init(KIS_req_header *hdr, uint8_t portal, uint16_t index, size_t argCount, size_t payloadSize, size_t rplWords)
{
    if (argCount > UINT8_MAX) {
        return IRECV_E_INVALID_INPUT;
    }

    if (index >= (1 << 10)) {
        return IRECV_E_INVALID_INPUT;
    }

    if (rplWords >= (1 << 14)) {
        return IRECV_E_INVALID_INPUT;
    }

    size_t reqSize = payloadSize + (argCount << 2);
    if (reqSize > UINT32_MAX) {
        return IRECV_E_INVALID_INPUT;
    }

    hdr->sequence         = 0; // Doesn't matter
    hdr->version          = 0xA0;
    hdr->portal           = portal;
    hdr->argCount         = (uint8_t) argCount;
    hdr->indexLo          = (uint8_t) (index & 0xFF);
    hdr->indexHiRplSizeLo = (uint8_t) (((index >> 8) & 0x3) | ((rplWords << 2) & 0xFC));
    hdr->rplSizeHi        = (uint8_t) ((rplWords >> 6) & 0xFF);
    hdr->reqSize          = (uint32_t) reqSize;

    return IRECV_E_SUCCESS;
}

static irecv_error_t irecv_kis_request(irecv_client_t client, KIS_req_header *req, size_t reqSize, KIS_req_header *rpl, size_t *rplSize)
{
    int endpoint = 0;
    switch (req->portal) {
        case KIS_PORTAL_CONFIG:
            endpoint = 1;
            break;
        case KIS_PORTAL_RSM:
            endpoint = 3;
            break;
        default:
            debug("Don't know which endpoint to use for portal %d\n", req->portal);
            return IRECV_E_INVALID_INPUT;
    }

    int sent = 0;
    irecv_error_t err = irecv_usb_bulk_transfer(client, endpoint, (unsigned char *) req, reqSize, &sent, USB_TIMEOUT);
    if (err != IRECV_E_SUCCESS) {
        debug("[send] irecv_usb_bulk_transfer failed, error %d\n", err);
        return err;
    }

    if ((size_t) sent != reqSize) {
        debug("sent != reqSize\n");
        return IRECV_E_USB_UPLOAD;
    }

    int rcvd = 0;
    err = irecv_usb_bulk_transfer(client, endpoint | 0x80, (unsigned char *) rpl, *rplSize, &rcvd, USB_TIMEOUT);
    if (err != IRECV_E_SUCCESS) {
        debug("[rcv] irecv_usb_bulk_transfer failed, error %d\n", err);
        return err;
    }

    *rplSize = rcvd;

    return IRECV_E_SUCCESS;
}

static irecv_error_t irecv_kis_config_write32(irecv_client_t client, uint8_t portal, uint16_t index, uint32_t value)
{
    KIS_config_wr32   req  = {};
    KIS_generic_reply rpl = {};
    irecv_error_t err = irecv_kis_request_init(&req.hdr, portal, index, 1, 0, 1);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to init KIS request, error %d\n", err);
        return err;
    }

    req.value = value;

    size_t rplSize = sizeof(rpl);
    err = irecv_kis_request(client, &req.hdr, sizeof(req), &rpl.hdr, &rplSize);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to send KIS request, error %d\n", err);
        return err;
    }

    if (rpl.size != 4) {
        debug("Failed to write config, %d bytes written, status %d\n", rpl.size, rpl.status);
        return err;
    }

    return IRECV_E_SUCCESS;
}

static int irecv_kis_read_string(KIS_device_info *di, size_t off, char *buf, size_t buf_size)
{
    off *= 4;

    size_t inputSize = sizeof(KIS_device_info) - sizeof(KIS_req_header);

    if ((off + 2) > inputSize)
        return 0;

    uint8_t len = di->deviceInfo[off];
    uint8_t type = di->deviceInfo[off + 1];

    if (len & 1)
        return 0;

    if (len/2 >= buf_size)
        return 0;

    if ((off + 2 + len) > inputSize)
        return 0;

    if (type != 3)
        return 0;

    buf[len >> 1] = 0;
    for (size_t i = 0; i < len; i += 2) {
        buf[i >> 1] = di->deviceInfo[i + off + 2];
    }

    return len/2;
}

static irecv_error_t irecv_kis_init(irecv_client_t client)
{
    irecv_error_t err = irecv_kis_config_write32(client, KIS_PORTAL_CONFIG, KIS_INDEX_ENABLE_A, KIS_ENABLE_A_VAL);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to write to KIS_INDEX_ENABLE_A, error %d\n", err);
        return err;
    }

    err = irecv_kis_config_write32(client, KIS_PORTAL_CONFIG, KIS_INDEX_ENABLE_B, KIS_ENABLE_B_VAL);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to write to KIS_INDEX_ENABLE_B, error %d\n", err);
        return err;
    }

    client->isKIS = 1;

    return IRECV_E_SUCCESS;
}

static irecv_error_t irecv_kis_load_device_info(irecv_client_t client)
{
    debug("Loading device info in KIS mode...\n");

    KIS_req_header req = {};
    KIS_device_info di = {};
    irecv_error_t err = irecv_kis_request_init(&req, KIS_PORTAL_RSM, KIS_INDEX_GET_INFO, 0, 0, sizeof(di.deviceInfo)/4);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to init KIS request, error %d\n", err);
        return err;
    }

    size_t rcvSize = sizeof(di);
    err = irecv_kis_request(client, &req, sizeof(req), &di.hdr, &rcvSize);
    if (err != IRECV_E_SUCCESS) {
        debug("Failed to send KIS request, error %d\n", err);
        return err;
    }

    char buf[0x100];
    int len = 0;

    len = irecv_kis_read_string(&di, di.deviceDescriptor.iSerialNumber, buf, sizeof(buf));
    if (len == 0)
        return IRECV_E_INVALID_INPUT;
    debug("Serial: %s\n", buf);

    irecv_load_device_info_from_iboot_string(client, buf);

    len = irecv_kis_read_string(&di, di.deviceDescriptor.iManufacturer, buf, sizeof(buf));
    if (len == 0)
        return IRECV_E_INVALID_INPUT;
    debug("Manufacturer: %s\n", buf);

    len = irecv_kis_read_string(&di, di.deviceDescriptor.iProduct, buf, sizeof(buf));
    if (len == 0)
        return IRECV_E_INVALID_INPUT;
    debug("Product: %s\n", buf);

    len = irecv_kis_read_string(&di, di.nonceOffset, buf, sizeof(buf));
    if (len == 0)
        return IRECV_E_INVALID_INPUT;
    debug("Nonces: %s\n", buf);

    irecv_copy_nonce_with_tag_from_buffer("NONC", &client->device_info.ap_nonce, &client->device_info.ap_nonce_size, buf);
    irecv_copy_nonce_with_tag_from_buffer("SNON", &client->device_info.sep_nonce, &client->device_info.sep_nonce_size, buf);

    debug("VID: 0x%04x\n", di.deviceDescriptor.idVendor);
    debug("PID: 0x%04x\n", di.deviceDescriptor.idProduct);

    client->mode  = di.deviceDescriptor.idProduct;

    return IRECV_E_SUCCESS;
}

static void iokit_cfdictionary_set_short(CFMutableDictionaryRef dict, const void *key, SInt16 value)
{
    CFNumberRef numberRef;

    numberRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberShortType, &value);
    if (numberRef) {
        CFDictionarySetValue(dict, key, numberRef);
        CFRelease(numberRef);
    }
}

static int check_context(irecv_client_t client)
{
    if (client == NULL || client->handle == NULL) {
        return IRECV_E_NO_DEVICE;
    }

    return IRECV_E_SUCCESS;
}

static int iokit_usb_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout)
{
    IOReturn result;
    IOUSBDevRequestTO req;

    bzero(&req, sizeof(req));
    req.bmRequestType     = bm_request_type;
    req.bRequest          = b_request;
    req.wValue            = OSSwapLittleToHostInt16(w_value);
    req.wIndex            = OSSwapLittleToHostInt16(w_index);
    req.wLength           = OSSwapLittleToHostInt16(w_length);
    req.pData             = data;
    req.noDataTimeout     = timeout;
    req.completionTimeout = timeout;

    result = (*client->handle)->DeviceRequestTO(client->handle, &req);
    switch (result) {
        case kIOReturnSuccess:         return req.wLenDone;
        case kIOReturnTimeout:         return IRECV_E_TIMEOUT;
#if !TARGET_OS_IPHONE
        case kIOUSBTransactionTimeout: return IRECV_E_TIMEOUT;
#endif
        case kIOReturnNotResponding:   return IRECV_E_NO_DEVICE;
        case kIOReturnNoDevice:           return IRECV_E_NO_DEVICE;
        default:
            return IRECV_E_UNKNOWN_ERROR;
    }
}

void dummy_callback(void) { }

int irecv_usb_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout)
{
    return iokit_usb_control_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, timeout);
}

static int iokit_usb_bulk_transfer(irecv_client_t client,
                        unsigned char endpoint,
                        unsigned char *data,
                        int length,
                        int *transferred,
                        unsigned int timeout)
{
    IOReturn result;
    IOUSBInterfaceInterface300 **intf = client->usbInterface;
    UInt32 size = length;
    UInt8 isUSBIn = (endpoint & kUSBbEndpointDirectionMask) != 0;
    UInt8 numEndpoints;

    if (!intf) return IRECV_E_USB_INTERFACE;

    result = (*intf)->GetNumEndpoints(intf, &numEndpoints);

    if (result != kIOReturnSuccess)
        return IRECV_E_USB_INTERFACE;

    for (UInt8 pipeRef = 0; pipeRef <= numEndpoints; pipeRef++) {
        UInt8 direction = 0;
        UInt8 number = 0;
        UInt8 transferType = 0;
        UInt16 maxPacketSize = 0;
        UInt8 interval = 0;

        result = (*intf)->GetPipeProperties(intf, pipeRef, &direction, &number, &transferType, &maxPacketSize, &interval);
        if (result != kIOReturnSuccess)
            continue;

        if (direction == 3)
            direction = isUSBIn;

        if (number != (endpoint & ~kUSBbEndpointDirectionMask) || direction != isUSBIn)
            continue;

        // Just because
        result = (*intf)->GetPipeStatus(intf, pipeRef);
        switch (result) {
            case kIOReturnSuccess:  break;
            case kIOReturnNoDevice: return IRECV_E_NO_DEVICE;
            case kIOReturnNotOpen:  return IRECV_E_UNABLE_TO_CONNECT;
            default:                return IRECV_E_USB_STATUS;
        }

        // Do the transfer
        if (isUSBIn) {
            result = (*intf)->ReadPipeTO(intf, pipeRef, data, &size, timeout, timeout);
            if (result != kIOReturnSuccess)
                return IRECV_E_PIPE;
            *transferred = size;

            return IRECV_E_SUCCESS;
        }
        else {
            // IOUSBInterfaceClass::interfaceWritePipe (intf?, pipeRef==1, data, size=0x8000)
            result = (*intf)->WritePipeTO(intf, pipeRef, data, size, timeout, timeout);
            if (result != kIOReturnSuccess)
                return IRECV_E_PIPE;
            *transferred = size;

            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_USB_INTERFACE;
}

int irecv_usb_bulk_transfer(irecv_client_t client,
                            unsigned char endpoint,
                            unsigned char *data,
                            int length,
                            int *transferred,
                            unsigned int timeout)
{
    return iokit_usb_bulk_transfer(client, endpoint, data, length, transferred, timeout);
}

static irecv_error_t iokit_usb_open_service(irecv_client_t *pclient, io_service_t service)
{
    IOReturn result;
    irecv_client_t client;
    SInt32 score;
    UInt16 mode;
    UInt32 locationID;
    IOCFPlugInInterface **plug = NULL;
    CFStringRef serialString;

    client = (irecv_client_t) calloc( 1, sizeof(struct irecv_client_private));

    // Create the plug-in
    result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (result != kIOReturnSuccess) {
        IOObjectRelease(service);
        free(client);
        return IRECV_E_UNKNOWN_ERROR;
    }

    // Cache the serial string before discarding the service. The service object
    // has a cached copy, so a request to the hardware device is not required.
    char serial_str[256];
    serial_str[0] = '\0';
    serialString = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBSerialNumberString), kCFAllocatorDefault, 0);
    if (serialString) {
        CFStringGetCString(serialString, serial_str, sizeof(serial_str), kCFStringEncodingUTF8);
        CFRelease(serialString);
    }
    irecv_load_device_info_from_iboot_string(client, serial_str);

    IOObjectRelease(service);

    // Create the device interface
    result = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&(client->handle));
    IODestroyPlugInInterface(plug);
    if (result != kIOReturnSuccess) {
        free(client);
        return IRECV_E_UNKNOWN_ERROR;
    }

    (*client->handle)->GetDeviceProduct(client->handle, &mode);
    (*client->handle)->GetLocationID(client->handle, &locationID);
    client->mode = mode;
    debug("opening device %04x:%04x @ %#010x...\n", kAppleVendorID, client->mode, (unsigned int)locationID);

    result = (*client->handle)->USBDeviceOpenSeize(client->handle);
    if (result != kIOReturnSuccess) {
        (*client->handle)->Release(client->handle);
        free(client);
        return IRECV_E_UNABLE_TO_CONNECT;
    }

    *pclient = client;
    return IRECV_E_SUCCESS;
}

static io_iterator_t iokit_usb_get_iterator_for_pid_and_class(UInt16 pid, const char *class)
{
    IOReturn result;
    io_iterator_t iterator;
    CFMutableDictionaryRef matchingDict;

    matchingDict = IOServiceMatching(class);
    iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBVendorID), kAppleVendorID);
    iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBProductID), pid);

    result = IOServiceGetMatchingServices(MACH_PORT_NULL, matchingDict, &iterator);
    if (result != kIOReturnSuccess)
        return IO_OBJECT_NULL;

    return iterator;
}

static io_iterator_t iokit_usb_get_iterator_for_pid(UInt16 pid)
{
    return iokit_usb_get_iterator_for_pid_and_class(pid, kIOUSBDeviceClassName);
}

static irecv_error_t iokit_open_with_ecid(irecv_client_t* pclient, uint64_t ecid)
{
    io_service_t service, ret_service;
    io_iterator_t iterator;
    CFStringRef usbSerial = NULL;
    CFStringRef ecidString = NULL;
    CFRange range;

    static const char *classes[] = { 
        kIOUSBDeviceClassName,
#if TARGET_OS_IPHONE
        "AppleEmbeddedUSBDevice",
#endif
        NULL
    };

    UInt16 wtf_pids[] = { IRECV_K_WTF_MODE, 0};
    UInt16 all_pids[] = { IRECV_K_WTF_MODE, IRECV_K_DFU_MODE, IRECV_K_PORT_DFU_MODE, IRECV_K_RECOVERY_MODE_1, IRECV_K_RECOVERY_MODE_2, IRECV_K_RECOVERY_MODE_3, IRECV_K_RECOVERY_MODE_4, KIS_PRODUCT_ID, 0 };
    UInt16 *pids = all_pids;
    int i;

    if (pclient == NULL) {
        debug("%s: pclient parameter is null\n", __func__);
        return IRECV_E_INVALID_INPUT;
    }
    if (ecid == IRECV_K_WTF_MODE) {
        /* special ecid case, ignore !IRECV_K_WTF_MODE */
        pids = wtf_pids;
        ecid = 0;
    }
    if (ecid > 0) {
        ecidString = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%llX"), ecid);
        if (ecidString == NULL) {
            debug("%s: failed to create ECID string\n", __func__);
            return IRECV_E_UNABLE_TO_CONNECT;
        }
    }

    *pclient = NULL;
    ret_service = IO_OBJECT_NULL;

    for (int c = 0; (classes[c] && ret_service == IO_OBJECT_NULL); c++) {
        for (i = 0; (pids[i] > 0 && ret_service == IO_OBJECT_NULL); i++) {
            iterator = iokit_usb_get_iterator_for_pid_and_class(pids[i], classes[c]);
            if (iterator) {
                while ((service = IOIteratorNext(iterator))) {

                    if (ecid == 0) {
                        ret_service = service;
                        break;
                    }

                    if (pids[i] == KIS_PRODUCT_ID) {
                        // In KIS Mode, we have to open the device in order to get
                        // it's ECID
                        irecv_error_t err = iokit_usb_open_service(pclient, service);
                        if (err != IRECV_E_SUCCESS) {
                            debug("%s: failed to open KIS device\n", __func__);
                            continue;
                        }

                        if (ecidString)
                            CFRelease(ecidString);

                        return IRECV_E_SUCCESS;
                    }

                    usbSerial = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBSerialNumberString), kCFAllocatorDefault, 0);
                    if (usbSerial == NULL) {
                        debug("%s: failed to create USB serial string property\n", __func__);
                        IOObjectRelease(service);
                        continue;
                    }

                    range = CFStringFind(usbSerial, ecidString, kCFCompareCaseInsensitive);
                    if (range.location == kCFNotFound) {
                        IOObjectRelease(service);
                    } else {
                        ret_service = service;
                        break;
                    }
                }
                if (usbSerial) {
                    CFRelease(usbSerial);
                    usbSerial = NULL;
                }
                IOObjectRelease(iterator);
            }
        }
    }

    if (ecidString)
        CFRelease(ecidString);

    if (ret_service == IO_OBJECT_NULL)
        return IRECV_E_UNABLE_TO_CONNECT;

    return iokit_usb_open_service(pclient, ret_service);
}

irecv_error_t irecv_open_with_ecid(irecv_client_t* pclient, uint64_t ecid)
{
    irecv_error_t error = IRECV_E_UNABLE_TO_CONNECT;

    if (libirecovery_debug) {
        irecv_set_debug_level(libirecovery_debug);
    }
    error = iokit_open_with_ecid(pclient, ecid);

    irecv_client_t client = *pclient;
    if (error != IRECV_E_SUCCESS) {
        irecv_close(client);
        return error;
    }

    error = irecv_usb_set_configuration(client, 1);
    if (error != IRECV_E_SUCCESS) {
        debug("Failed to set configuration, error %d\n", error);
        irecv_close(client);
        return error;
    }

    if (client->mode == IRECV_K_DFU_MODE || client->mode == IRECV_K_PORT_DFU_MODE || client->mode == IRECV_K_WTF_MODE || client->mode == KIS_PRODUCT_ID) {
        error = irecv_usb_set_interface(client, 0, 0);
    } else {
        error = irecv_usb_set_interface(client, 0, 0);
        if (error == IRECV_E_SUCCESS && client->mode > IRECV_K_RECOVERY_MODE_2) {
            error = irecv_usb_set_interface(client, 1, 1);
        }
    }

    if (error != IRECV_E_SUCCESS) {
        debug("Failed to set interface, error %d\n", error);
        irecv_close(client);
        return error;
    }

    if (client->mode == KIS_PRODUCT_ID) {
        error = irecv_kis_init(client);
        if (error != IRECV_E_SUCCESS) {
            debug("irecv_kis_init failed, error %d\n", error);
            irecv_close(client);
            return error;
        }

        error = irecv_kis_load_device_info(client);
        if (error != IRECV_E_SUCCESS) {
            debug("irecv_kis_load_device_info failed, error %d\n", error);
            irecv_close(client);
            return error;
        }
        if (ecid != 0 && client->device_info.ecid != ecid) {
            irecv_close(client);
            return IRECV_E_NO_DEVICE; //wrong device
        }
        debug("found device with ECID %016" PRIx64 "\n", (uint64_t)client->device_info.ecid);
    } else {
        irecv_copy_nonce_with_tag(client, "NONC", &client->device_info.ap_nonce, &client->device_info.ap_nonce_size);
        irecv_copy_nonce_with_tag(client, "SNON", &client->device_info.sep_nonce, &client->device_info.sep_nonce_size);
    }

    if (error == IRECV_E_SUCCESS) {
        if ((*pclient)->connected_callback != NULL) {
            irecv_event_t event;
            event.size = 0;
            event.data = NULL;
            event.progress = 0;
            event.type = IRECV_CONNECTED;
            (*pclient)->connected_callback(*pclient, &event);
        }
    }
    return error;
}

irecv_error_t irecv_usb_set_configuration(irecv_client_t client, int configuration)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    debug("Setting to configuration %d\n", configuration);

    IOReturn result;

    result = (*client->handle)->SetConfiguration(client->handle, configuration);
    if (result != kIOReturnSuccess) {
        debug("error setting configuration: %#x\n", result);
        return IRECV_E_USB_CONFIGURATION;
    }

    client->usb_config = configuration;

    return IRECV_E_SUCCESS;
}

static IOReturn iokit_usb_get_interface(IOUSBDeviceInterface320 **device, uint8_t ifc, io_service_t *usbInterfacep)
{
    IOUSBFindInterfaceRequest request;
    uint8_t                   current_interface;
    kern_return_t             kresult;
    io_iterator_t             interface_iterator;

    *usbInterfacep = IO_OBJECT_NULL;

    request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;

    kresult = (*device)->CreateInterfaceIterator(device, &request, &interface_iterator);
    if (kresult)
        return kresult;

    for ( current_interface = 0 ; current_interface <= ifc ; current_interface++ ) {
        *usbInterfacep = IOIteratorNext(interface_iterator);
        if (current_interface != ifc)
            (void) IOObjectRelease (*usbInterfacep);
    }
    IOObjectRelease(interface_iterator);

    return kIOReturnSuccess;
}

static irecv_error_t iokit_usb_set_interface(irecv_client_t client, int usb_interface, int usb_alt_interface)
{
    IOReturn result;
    io_service_t interface_service = IO_OBJECT_NULL;
    IOCFPlugInInterface **plugInInterface = NULL;
    SInt32 score;

    // Close current interface
    if (client->usbInterface) {
        result = (*client->usbInterface)->USBInterfaceClose(client->usbInterface);
        result = (*client->usbInterface)->Release(client->usbInterface);
        client->usbInterface = NULL;
    }

    result = iokit_usb_get_interface(client->handle, usb_interface, &interface_service);
    if (result != kIOReturnSuccess) {
        debug("failed to find requested interface: %d\n", usb_interface);
        return IRECV_E_USB_INTERFACE;
    }

    result = IOCreatePlugInInterfaceForService(interface_service, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plugInInterface, &score);
    IOObjectRelease(interface_service);
    if (result != kIOReturnSuccess) {
        debug("error creating plug-in interface: %#x\n", result);
        return IRECV_E_USB_INTERFACE;
    }

    result = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID)&client->usbInterface);
    IODestroyPlugInInterface(plugInInterface);
    if (result != kIOReturnSuccess) {
        debug("error creating interface interface: %#x\n", result);
        return IRECV_E_USB_INTERFACE;
    }

    result = (*client->usbInterface)->USBInterfaceOpen(client->usbInterface);
    if (result != kIOReturnSuccess) {
        debug("error opening interface: %#x\n", result);
        return IRECV_E_USB_INTERFACE;
    }

    if (usb_interface == 1) {
        result = (*client->usbInterface)->SetAlternateInterface(client->usbInterface, usb_alt_interface);
        if (result != kIOReturnSuccess) {
            debug("error setting alternate interface: %#x\n", result);
            return IRECV_E_USB_INTERFACE;
        }
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_usb_set_interface(irecv_client_t client, int usb_interface, int usb_alt_interface)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    debug("Setting to interface %d:%d\n", usb_interface, usb_alt_interface);

    if (iokit_usb_set_interface(client, usb_interface, usb_alt_interface) < 0) {
        return IRECV_E_USB_INTERFACE;
    }

    client->usb_interface = usb_interface;
    client->usb_alt_interface = usb_alt_interface;

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_reset(irecv_client_t client)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    IOReturn result;

    result = (*client->handle)->ResetDevice(client->handle);
    if (result != kIOReturnSuccess && result != kIOReturnNotResponding) {
        debug("error sending device reset: %#x\n", result);
        return IRECV_E_UNKNOWN_ERROR;
    }

    result = (*client->handle)->USBDeviceReEnumerate(client->handle, 0);
    if (result != kIOReturnSuccess && result != kIOReturnNotResponding) {
        debug("error re-enumerating device: %#x (ignored)\n", result);
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_open_with_ecid_and_attempts(irecv_client_t* pclient, uint64_t ecid, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (*pclient) {
            irecv_close(*pclient);
            *pclient = NULL;
        }
        if (irecv_open_with_ecid(pclient, ecid) != IRECV_E_SUCCESS) {
            debug("Connection failed. Waiting 1 sec before retry.\n");
            sleep(1);
        } else {
            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_UNABLE_TO_CONNECT;
}

irecv_error_t irecv_event_subscribe(irecv_client_t client, irecv_event_type type, irecv_event_cb_t callback, void* user_data)
{
	switch(type) {
	case IRECV_RECEIVED:
		client->received_callback = callback;
		break;

	case IRECV_PROGRESS:
		client->progress_callback = callback;
		break;

	case IRECV_CONNECTED:
		client->connected_callback = callback;
		break;

	case IRECV_PRECOMMAND:
		client->precommand_callback = callback;
		break;

	case IRECV_POSTCOMMAND:
		client->postcommand_callback = callback;
		break;

	case IRECV_DISCONNECTED:
		client->disconnected_callback = callback;
		break;

	default:
		return IRECV_E_UNKNOWN_ERROR;
	}

	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_event_unsubscribe(irecv_client_t client, irecv_event_type type)
{
	switch(type) {
	case IRECV_RECEIVED:
		client->received_callback = NULL;
		break;

	case IRECV_PROGRESS:
		client->progress_callback = NULL;
		break;

	case IRECV_CONNECTED:
		client->connected_callback = NULL;
		break;

	case IRECV_PRECOMMAND:
		client->precommand_callback = NULL;
		break;

	case IRECV_POSTCOMMAND:
		client->postcommand_callback = NULL;
		break;

	case IRECV_DISCONNECTED:
		client->disconnected_callback = NULL;
		break;

	default:
		return IRECV_E_UNKNOWN_ERROR;
	}

	return IRECV_E_SUCCESS;
}

irecv_error_t irecv_close(irecv_client_t client)
{
    if (client != NULL) {
        if (client->disconnected_callback != NULL) {
            irecv_event_t event;
            event.size = 0;
            event.data = NULL;
            event.progress = 0;
            event.type = IRECV_DISCONNECTED;
            client->disconnected_callback(client, &event);
        }

        if (client->usbInterface) {
            (*client->usbInterface)->USBInterfaceClose(client->usbInterface);
            (*client->usbInterface)->Release(client->usbInterface);
            client->usbInterface = NULL;
        }
        if (client->handle) {
            (*client->handle)->USBDeviceClose(client->handle);
            (*client->handle)->Release(client->handle);
            client->handle = NULL;
        }

        free(client->device_info.srnm);
        free(client->device_info.imei);
        free(client->device_info.srtg);
        free(client->device_info.serial_string);
        free(client->device_info.ap_nonce);
        free(client->device_info.sep_nonce);

        free(client);
        client = NULL;
    }

    return IRECV_E_SUCCESS;
}

void irecv_set_debug_level(int level)
{
    libirecovery_debug = level;
}

// const char* irecv_version()
// {
// #ifndef PACKAGE_VERSION
// #error PACKAGE_VERSION is not defined!
// #endif
//     return PACKAGE_VERSION;
// }

static irecv_error_t irecv_send_command_raw(irecv_client_t client, const char* command, uint8_t b_request)
{
    unsigned int length = strlen(command);
    if (length >= 0x100) {
        return IRECV_E_INVALID_INPUT;
    }

    if (length > 0) {
        irecv_usb_control_transfer(client, 0x40, b_request, 0, 0, (unsigned char*) command, length + 1, USB_TIMEOUT);
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_command_breq(irecv_client_t client, const char* command, uint8_t b_request)
{
    irecv_error_t error = 0;

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    unsigned int length = strlen(command);
    if (length >= 0x100) {
        return IRECV_E_INVALID_INPUT;
    }

    irecv_event_t event;
    if (client->precommand_callback != NULL) {
        event.size = length;
        event.data = command;
        event.type = IRECV_PRECOMMAND;
        if (client->precommand_callback(client, &event)) {
            return IRECV_E_SUCCESS;
        }
    }

    error = irecv_send_command_raw(client, command, b_request);
    if (error != IRECV_E_SUCCESS) {
        debug("Failed to send command %s\n", command);
        if (error != IRECV_E_PIPE)
            return error;
    }

    if (client->postcommand_callback != NULL) {
        event.size = length;
        event.data = command;
        event.type = IRECV_POSTCOMMAND;
        if (client->postcommand_callback(client, &event)) {
            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_command(irecv_client_t client, const char* command)
{
    return irecv_send_command_breq(client, command, 0);
}

irecv_error_t irecv_send_file(irecv_client_t client, const char* filename, unsigned int options)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        return IRECV_E_FILE_NOT_FOUND;
    }

    struct stat fst;
    if (fstat(fileno(file), &fst) < 0) {
        return IRECV_E_UNKNOWN_ERROR;
    }
    size_t length = fst.st_size;

    char* buffer = (char*)malloc(length);
    if (buffer == NULL) {
        fclose(file);
        return IRECV_E_OUT_OF_MEMORY;
    }

    size_t bytes = fread(buffer, 1, length, file);
    fclose(file);

    if (bytes != length) {
        free(buffer);
        return IRECV_E_UNKNOWN_ERROR;
    }

    irecv_error_t error = irecv_send_buffer(client, (unsigned char*)buffer, length, options);
    free(buffer);

    return error;
}

static irecv_error_t irecv_get_status(irecv_client_t client, unsigned int* status)
{
    if (check_context(client) != IRECV_E_SUCCESS) {
        *status = 0;
        return IRECV_E_NO_DEVICE;
    }

    unsigned char buffer[6];
    memset(buffer, '\0', 6);
    if (irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, buffer, 6, USB_TIMEOUT) != 6) {
        *status = 0;
        return IRECV_E_USB_STATUS;
    }

    *status = (unsigned int) buffer[4];

    return IRECV_E_SUCCESS;
}

static irecv_error_t irecv_kis_send_buffer(irecv_client_t client, unsigned char* buffer, unsigned long length, unsigned int options)
{
    if (client->mode != IRECV_K_DFU_MODE) {
        return IRECV_E_UNSUPPORTED;
    }

    unsigned long origLen = length;

    KIS_upload_chunk *chunk = calloc(1, sizeof(KIS_upload_chunk));
    uint64_t address = 0;
    while (length) {
        unsigned long toUpload = length;
        if (toUpload > 0x4000)
            toUpload = 0x4000;

        irecv_error_t error = irecv_kis_request_init(&chunk->hdr, KIS_PORTAL_RSM, KIS_INDEX_UPLOAD, 3, toUpload, 0);
        if (error != IRECV_E_SUCCESS) {
            free(chunk);
            debug("Failed to init chunk header, error %d\n", error);
            return error;
        }

        chunk->address = address;
        chunk->size    = toUpload;
        memcpy(chunk->data, buffer, toUpload);

        KIS_generic_reply reply;
        size_t rcvSize = sizeof(reply);
        error = irecv_kis_request(client, &chunk->hdr, sizeof(*chunk) - (0x4000 - toUpload), &reply.hdr, &rcvSize);

        if (error != IRECV_E_SUCCESS) {
            free(chunk);
            debug("Failed to upload chunk, error %d\n", error);
            return error;
        }

        address += toUpload;
        buffer  += toUpload;
        length  -= toUpload;

        if (client->progress_callback != NULL) {
            irecv_event_t event;
            event.progress = ((double) (origLen - length) / (double) origLen) * 100.0;
            event.type = IRECV_PROGRESS;
            event.data = (char*)"Uploading";
            event.size = origLen - length;
            client->progress_callback(client, &event);
        } else {
            debug("Sent: %lu bytes - %lu of %lu\n", toUpload, origLen - length, origLen);
        }
    }
    free(chunk);

    if (options & IRECV_SEND_OPT_DFU_NOTIFY_FINISH) {
        irecv_error_t error = irecv_kis_config_write32(client, KIS_PORTAL_RSM, KIS_INDEX_BOOT_IMG, origLen);

        if (error != IRECV_E_SUCCESS) {
            debug("Failed to boot image, error %d\n", error);
            return error;
        }
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_send_buffer(irecv_client_t client, unsigned char* buffer, unsigned long length, unsigned int options)
{
    if (client->isKIS)
        return irecv_kis_send_buffer(client, buffer, length, options);

    irecv_error_t error = 0;
    int recovery_mode = ((client->mode != IRECV_K_DFU_MODE) && (client->mode != IRECV_K_PORT_DFU_MODE) && (client->mode != IRECV_K_WTF_MODE));

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    unsigned int h1 = 0xFFFFFFFF;
    unsigned char dfu_xbuf[12] = {0xff, 0xff, 0xff, 0xff, 0xac, 0x05, 0x00, 0x01, 0x55, 0x46, 0x44, 0x10};
    int dfu_crc = 1;
    int packet_size = recovery_mode ? 0x8000 : 0x800;
    if (!recovery_mode && (options & IRECV_SEND_OPT_DFU_SMALL_PKT)) {
        packet_size = 0x40;
        dfu_crc = 0;
    }
    int last = length % packet_size;
    int packets = length / packet_size;

    if (last != 0) {
        packets++;
    } else {
        last = packet_size;
    }

    /* initiate transfer */
    if (recovery_mode) {
        error = irecv_usb_control_transfer(client, 0x41, 0, 0, 0, NULL, 0, USB_TIMEOUT);
    } else {
        uint8_t state = 0;
        if (irecv_usb_control_transfer(client, 0xa1, 5, 0, 0, (unsigned char*)&state, 1, USB_TIMEOUT) == 1) {
            error = IRECV_E_SUCCESS;
        } else {
            return IRECV_E_USB_UPLOAD;
        }
        switch (state) {
        case 2:
            /* DFU IDLE */
            break;
        case 10:
            debug("DFU ERROR, issuing CLRSTATUS\n");
            irecv_usb_control_transfer(client, 0x21, 4, 0, 0, NULL, 0, USB_TIMEOUT);
            error = IRECV_E_USB_UPLOAD;
            break;
        default:
            debug("Unexpected state %d, issuing ABORT\n", state);
            irecv_usb_control_transfer(client, 0x21, 6, 0, 0, NULL, 0, USB_TIMEOUT);
            error = IRECV_E_USB_UPLOAD;
            break;
        }
    }

    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    int i = 0;
    unsigned long count = 0;
    unsigned int status = 0;
    int bytes = 0;
    for (i = 0; i < packets; i++) {
        int size = (i + 1) < packets ? packet_size : last;

        /* Use bulk transfer for recovery mode and control transfer for DFU and WTF mode */
        if (recovery_mode) {
            error = irecv_usb_bulk_transfer(client, 0x04, &buffer[i * packet_size], size, &bytes, USB_TIMEOUT);
        } else {
            if (dfu_crc) {
                int j;
                for (j = 0; j < size; j++) {
                    crc32_step(h1, buffer[i*packet_size + j]);
                }
            }
            if (dfu_crc && i+1 == packets) {
                int j;
                if (size+16 > packet_size) {
                    bytes = irecv_usb_control_transfer(client, 0x21, 1, i, 0, &buffer[i * packet_size], size, USB_TIMEOUT);
                    if (bytes != size) {
                        return IRECV_E_USB_UPLOAD;
                    }
                    count += size;
                    size = 0;
                }
                for (j = 0; j < 2; j++) {
                    crc32_step(h1, dfu_xbuf[j*6 + 0]);
                    crc32_step(h1, dfu_xbuf[j*6 + 1]);
                    crc32_step(h1, dfu_xbuf[j*6 + 2]);
                    crc32_step(h1, dfu_xbuf[j*6 + 3]);
                    crc32_step(h1, dfu_xbuf[j*6 + 4]);
                    crc32_step(h1, dfu_xbuf[j*6 + 5]);
                }

                char* newbuf = (char*)malloc(size + 16);
                if (size > 0) {
                    memcpy(newbuf, &buffer[i * packet_size], size);
                }
                memcpy(newbuf+size, dfu_xbuf, 12);
                newbuf[size+12] = h1 & 0xFF;
                newbuf[size+13] = (h1 >> 8) & 0xFF;
                newbuf[size+14] = (h1 >> 16) & 0xFF;
                newbuf[size+15] = (h1 >> 24) & 0xFF;
                size += 16;
                bytes = irecv_usb_control_transfer(client, 0x21, 1, i, 0, (unsigned char*)newbuf, size, USB_TIMEOUT);
                free(newbuf);
            } else {
                bytes = irecv_usb_control_transfer(client, 0x21, 1, i, 0, &buffer[i * packet_size], size, USB_TIMEOUT);
            }
        }

        if (bytes != size) {
            return IRECV_E_USB_UPLOAD;
        }

        if (!recovery_mode) {
            error = irecv_get_status(client, &status);
        }

        if (error != IRECV_E_SUCCESS) {
            return error;
        }

        if (!recovery_mode && status != 5) {
            int retry = 0;

            while (retry++ < 20) {
                irecv_get_status(client, &status);
                if (status == 5) {
                    break;
                }
                sleep(1);
            }

            if (status != 5) {
                return IRECV_E_USB_UPLOAD;
            }
        }

        count += size;
        if (client->progress_callback != NULL) {
            irecv_event_t event;
            event.progress = ((double) count/ (double) length) * 100.0;
            event.type = IRECV_PROGRESS;
            event.data = (char*)"Uploading";
            event.size = count;
            client->progress_callback(client, &event);
        } else {
            debug("Sent: %d bytes - %lu of %lu\n", bytes, count, length);
        }
    }

    if (recovery_mode && length % 512 == 0) {
        /* send a ZLP */
        bytes = 0;
        irecv_usb_bulk_transfer(client, 0x04, buffer, 0, &bytes, USB_TIMEOUT);
    }

    if ((options & IRECV_SEND_OPT_DFU_NOTIFY_FINISH) && !recovery_mode) {
        irecv_usb_control_transfer(client, 0x21, 1, packets, 0, (unsigned char*) buffer, 0, USB_TIMEOUT);

        for (i = 0; i < 2; i++) {
            error = irecv_get_status(client, &status);
            if (error != IRECV_E_SUCCESS) {
                return error;
            }
        }

        if ((options & IRECV_SEND_OPT_DFU_FORCE_ZLP)) {
            /* we send a pseudo ZLP here just in case */
            irecv_usb_control_transfer(client, 0x21, 1, 0, 0, 0, 0, USB_TIMEOUT);
        }

        irecv_reset(client);
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_receive(irecv_client_t client)
{
    char buffer[BUFFER_SIZE];
    memset(buffer, '\0', BUFFER_SIZE);

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    int bytes = 0;
    while (1) {
        irecv_usb_set_interface(client, 1, 1);
        int r = irecv_usb_bulk_transfer(client, 0x81, (unsigned char*) buffer, BUFFER_SIZE, &bytes, 500);
        irecv_usb_set_interface(client, 0, 0);
        if (r != 0) {
            break;
        }
        if (bytes > 0) {
            if (client->received_callback != NULL) {
                irecv_event_t event;
                event.size = bytes;
                event.data = buffer;
                event.type = IRECV_RECEIVED;
                if (client->received_callback(client, &event) != 0) {
                    break;
                }
            }
        } else break;
    }
    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_getenv(irecv_client_t client, const char* variable, char** value)
{
    char command[256];

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    *value = NULL;

    if (variable == NULL) {
        return IRECV_E_INVALID_INPUT;
    }

    memset(command, '\0', sizeof(command));
    snprintf(command, sizeof(command)-1, "getenv %s", variable);
    irecv_error_t error = irecv_send_command_raw(client, command, 0);
    if (error == IRECV_E_PIPE) {
        return IRECV_E_SUCCESS;
    }

    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    char* response = (char*) malloc(256);
    if (response == NULL) {
        return IRECV_E_OUT_OF_MEMORY;
    }

    memset(response, '\0', 256);
    irecv_usb_control_transfer(client, 0xC0, 0, 0, 0, (unsigned char*) response, 255, USB_TIMEOUT);

    *value = response;

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_getret(irecv_client_t client, unsigned int* value)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    *value = 0;

    char* response = (char*) malloc(256);
    if (response == NULL) {
        return IRECV_E_OUT_OF_MEMORY;
    }

    memset(response, '\0', 256);
    irecv_usb_control_transfer(client, 0xC0, 0, 0, 0, (unsigned char*) response, 255, USB_TIMEOUT);

    *value = (unsigned int) *response;

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_get_mode(irecv_client_t client, int* mode)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    *mode = client->mode;

    return IRECV_E_SUCCESS;
}

const struct irecv_device_info* irecv_get_device_info(irecv_client_t client)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return NULL;

    return &client->device_info;
}

irecv_error_t irecv_execute_script(irecv_client_t client, const char* script)
{
    irecv_error_t error = IRECV_E_SUCCESS;
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    char* body = strdup(script);
    char* line = strtok(body, "\n");

    while (line != NULL) {
        if (line[0] != '#') {
            error = irecv_send_command(client, line);
            if (error != IRECV_E_SUCCESS) {
                break;
            }

            error = irecv_receive(client);
            if (error != IRECV_E_SUCCESS) {
                break;
            }
        }
        line = strtok(NULL, "\n");
    }

    free(body);

    return error;
}

irecv_error_t irecv_saveenv(irecv_client_t client)
{
    irecv_error_t error = irecv_send_command_raw(client, "saveenv", 0);
    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_setenv(irecv_client_t client, const char* variable, const char* value)
{
    char command[256];

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    if (variable == NULL || value == NULL) {
        return IRECV_E_UNKNOWN_ERROR;
    }

    memset(command, '\0', sizeof(command));
    snprintf(command, sizeof(command)-1, "setenv %s %s", variable, value);
    irecv_error_t error = irecv_send_command_raw(client, command, 0);
    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_setenv_np(irecv_client_t client, const char* variable, const char* value)
{
    char command[256];

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    if (variable == NULL || value == NULL) {
        return IRECV_E_UNKNOWN_ERROR;
    }

    memset(command, '\0', sizeof(command));
    snprintf(command, sizeof(command)-1, "setenvnp %s %s", variable, value);
    irecv_error_t error = irecv_send_command_raw(client, command, 0);
    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_reboot(irecv_client_t client)
{
    irecv_error_t error = irecv_send_command_raw(client, "reboot", 0);
    if (error != IRECV_E_SUCCESS) {
        return error;
    }

    return IRECV_E_SUCCESS;
}

const char* irecv_strerror(irecv_error_t error)
{
    switch (error) {
    case IRECV_E_SUCCESS:
        return "Command completed successfully";

    case IRECV_E_NO_DEVICE:
        return "Unable to find device";

    case IRECV_E_OUT_OF_MEMORY:
        return "Out of memory";

    case IRECV_E_UNABLE_TO_CONNECT:
        return "Unable to connect to device";

    case IRECV_E_INVALID_INPUT:
        return "Invalid input";

    case IRECV_E_FILE_NOT_FOUND:
        return "File not found";

    case IRECV_E_USB_UPLOAD:
        return "Unable to upload data to device";

    case IRECV_E_USB_STATUS:
        return "Unable to get device status";

    case IRECV_E_USB_INTERFACE:
        return "Unable to set device interface";

    case IRECV_E_USB_CONFIGURATION:
        return "Unable to set device configuration";

    case IRECV_E_PIPE:
        return "Broken pipe";

    case IRECV_E_TIMEOUT:
        return "Timeout talking to device";

    case IRECV_E_UNSUPPORTED:
        return "Operation unsupported by driver";

    default:
        return "Unknown error";
    }

    return NULL;
}

irecv_error_t irecv_reset_counters(irecv_client_t client)
{
    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    if ((client->mode == IRECV_K_DFU_MODE) || (client->mode == IRECV_K_PORT_DFU_MODE) || (client->mode == IRECV_K_WTF_MODE)) {
        irecv_usb_control_transfer(client, 0x21, 4, 0, 0, 0, 0, USB_TIMEOUT);
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_recv_buffer(irecv_client_t client, char* buffer, unsigned long length)
{
    int recovery_mode = ((client->mode != IRECV_K_DFU_MODE) && (client->mode != IRECV_K_PORT_DFU_MODE) && (client->mode != IRECV_K_WTF_MODE));

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    int packet_size = recovery_mode ? 0x2000: 0x800;
    int last = length % packet_size;
    int packets = length / packet_size;
    if (last != 0) {
        packets++;
    } else {
        last = packet_size;
    }

    int i = 0;
    int bytes = 0;
    unsigned long count = 0;
    for (i = 0; i < packets; i++) {
        unsigned short size = (i+1) < packets ? packet_size : last;
        bytes = irecv_usb_control_transfer(client, 0xA1, 2, 0, 0, (unsigned char*)&buffer[i * packet_size], size, USB_TIMEOUT);

        if (bytes != size) {
            return IRECV_E_USB_UPLOAD;
        }

        count += size;
        if (client->progress_callback != NULL) {
            irecv_event_t event;
            event.progress = ((double) count/ (double) length) * 100.0;
            event.type = IRECV_PROGRESS;
            event.data = (char*)"Downloading";
            event.size = count;
            client->progress_callback(client, &event);
        } else {
            debug("Sent: %d bytes - %lu of %lu\n", bytes, count, length);
        }
    }

    return IRECV_E_SUCCESS;
}

irecv_error_t irecv_finish_transfer(irecv_client_t client)
{
    int i = 0;
    unsigned int status = 0;

    if (check_context(client) != IRECV_E_SUCCESS)
        return IRECV_E_NO_DEVICE;

    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, 0, 0, USB_TIMEOUT);

    for (i = 0; i < 3; i++){
        irecv_get_status(client, &status);
    }

    irecv_reset(client);

    return IRECV_E_SUCCESS;
}

irecv_device_t irecv_devices_get_all(void)
{
    return irecv_devices;
}

irecv_error_t irecv_devices_get_device_by_client(irecv_client_t client, irecv_device_t* device)
{
    int i = 0;

    if (!client || !device)
        return IRECV_E_INVALID_INPUT;

    *device = NULL;

    if (client->device_info.cpid == 0) {
        return IRECV_E_UNKNOWN_ERROR;
    }

    unsigned int cpid_match = client->device_info.cpid;
    unsigned int bdid_match = client->device_info.bdid;
    if (client->mode == IRECV_K_PORT_DFU_MODE) {
        cpid_match = (client->device_info.bdid >> 8) & 0xFFFF;
        bdid_match = (client->device_info.bdid >> 24) & 0xFF;
    }

    for (i = 0; irecv_devices[i].hardware_model != NULL; i++) {
        if (irecv_devices[i].chip_id == cpid_match && irecv_devices[i].board_id == bdid_match) {
            *device = &irecv_devices[i];
            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_NO_DEVICE;
}

irecv_error_t irecv_devices_get_device_by_product_type(const char* product_type, irecv_device_t* device)
{
    int i = 0;

    if (!product_type || !device)
        return IRECV_E_INVALID_INPUT;

    *device = NULL;

    for (i = 0; irecv_devices[i].product_type != NULL; i++) {
        if (!strcmp(product_type, irecv_devices[i].product_type)) {
            *device = &irecv_devices[i];
            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_NO_DEVICE;
}

irecv_error_t irecv_devices_get_device_by_hardware_model(const char* hardware_model, irecv_device_t* device)
{
    int i = 0;

    if (!hardware_model || !device)
        return IRECV_E_INVALID_INPUT;

    *device = NULL;

    for (i = 0; irecv_devices[i].hardware_model != NULL; i++) {
        if (!strcasecmp(hardware_model, irecv_devices[i].hardware_model)) {
            *device = &irecv_devices[i];
            return IRECV_E_SUCCESS;
        }
    }

    return IRECV_E_NO_DEVICE;
}

irecv_client_t irecv_reconnect(irecv_client_t client, int initial_pause)
{
    irecv_error_t error = 0;
    irecv_client_t new_client = NULL;
    irecv_event_cb_t progress_callback = client->progress_callback;
    irecv_event_cb_t received_callback = client->received_callback;
    irecv_event_cb_t connected_callback = client->connected_callback;
    irecv_event_cb_t precommand_callback = client->precommand_callback;
    irecv_event_cb_t postcommand_callback = client->postcommand_callback;
    irecv_event_cb_t disconnected_callback = client->disconnected_callback;

    uint64_t ecid = client->device_info.ecid;

    if (check_context(client) == IRECV_E_SUCCESS) {
        irecv_close(client);
    }

    if (initial_pause > 0) {
        debug("Waiting %d seconds for the device to pop up...\n", initial_pause);
        sleep(initial_pause);
    }

    error = irecv_open_with_ecid_and_attempts(&new_client, ecid, 10);
    if (error != IRECV_E_SUCCESS) {
        return NULL;
    }

    new_client->progress_callback = progress_callback;
    new_client->received_callback = received_callback;
    new_client->connected_callback = connected_callback;
    new_client->precommand_callback = precommand_callback;
    new_client->postcommand_callback = postcommand_callback;
    new_client->disconnected_callback = disconnected_callback;

    if (new_client->connected_callback != NULL) {
        irecv_event_t event;
        event.size = 0;
        event.data = NULL;
        event.progress = 0;
        event.type = IRECV_CONNECTED;
        new_client->connected_callback(new_client, &event);
    }

    return new_client;
}

/* additional stuff */
void async_dummy_callback(void *refcon, IOReturn result, void *arg0) {
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static int iokit_usb_dummy_async_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout)
{
    IOReturn result;
    IOUSBDevRequestTO req;
    CFRunLoopSourceRef async_event_source;

    (*client->handle)->CreateDeviceAsyncEventSource(client->handle, &async_event_source);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), async_event_source, kCFRunLoopDefaultMode);

    bzero(&req, sizeof(req));
    req.bmRequestType     = bm_request_type;
    req.bRequest          = b_request;
    req.wValue            = OSSwapLittleToHostInt16(w_value);
    req.wIndex            = OSSwapLittleToHostInt16(w_index);
    req.wLength           = OSSwapLittleToHostInt16(w_length);
    req.pData             = data;
    req.noDataTimeout     = timeout;
    req.completionTimeout = timeout;

    result = (*client->handle)->DeviceRequestAsyncTO(client->handle, &req, async_dummy_callback, NULL);
    (*client->handle)->USBDeviceAbortPipeZero(client->handle);

    CFRunLoopRun();

    switch (result) {
        case kIOReturnSuccess:         return req.wLenDone;
        case kIOReturnTimeout:         return IRECV_E_TIMEOUT;
#if !TARGET_OS_IPHONE
        case kIOUSBTransactionTimeout: return IRECV_E_TIMEOUT;
#endif
        case kIOReturnNotResponding:   return IRECV_E_NO_DEVICE;
        case kIOReturnNoDevice:        return IRECV_E_NO_DEVICE;
        default:
            return IRECV_E_UNKNOWN_ERROR;
    }
}

int irecv_usb_dummy_async_control_transfer(irecv_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int timeout) {
	return iokit_usb_dummy_async_control_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, timeout);
}