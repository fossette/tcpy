# tcpy
Timed Copy

## Purpose

Copy and verify files, and also let hard disks some time to perform their internal disk operations when dealing with huge files.  Also, the copy process can easily be paused or cancelled.  Files can be copied.  Files can moved using the **-del** parameter.  There's also the possibility to mirror two directories using the **-mir** parameter.

Accepted keyboard keys are:
- ESC, Q : Quit the copy process
- SPACE : Pause the copy process
- V : Pause after the verify process

## Work in progress

The initial releases were abandoned because first, I used the name 'tcopy' which is already being used in the system files for tape copying.  Also, I had a '.tcopy' directory resource file that was meant as a helper to synchronize files, but the mere presence of this file caused issues with network drives.  So, I removed that directory resource feature that should be part of the file system anyway.

All that being said, such a drastic change may have introduces some bugs, so the USE AT YOUR OWN RISK warning is more important than ever.  Tests will be sustained over time, but for me, this program serves its purpose.

## How to build and install tcpy
1. Download the source files and store them in a directory
2. Go to that directory in a terminal window
3. To built the executable file, type `make`
4. To install the executable file, type `make install` as a superuser.  The Makefile will copy the executable file into the `/usr/bin` directory.  If you want it elsewhere, feel free to copy it by hand instead.

## Version history
1.0 - 2020/07/22 - Initial release - Abandoned

2.0 - 2021/02/22 - Abandoned

3.0 - 2022/06/12 - Official release (LOL!!!)

## Compatibility
**tcpy** has been tested under FreeBSD 12.2.  Should be easy to port because there are few dependencies.

## Donations
Thanks for the support!  
Bitcoin: **1JbiV7rGE5kRKcecTfPv16SXag65o8aQTe**

# Have Fun!
