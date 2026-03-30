# Project Name

Map mouse/touchpot to keyboard, enable to use simulated mouse/touchpot in chrome on tvbox/4.4.2/ndk-r10e

## Setup
- OS: Ubuntu 24.04/x86
  Target: Android/4.4.2, ndk-r10e
  Use toolchain from ndk-r10e for arm to do cross-compile

## Installation
```bash
1) compile from source or copy the prebuilt bin 
2) copy bin to /system/bin/, make it run at boot, e.g. add to install-recovery.sh, as it run in system service
3) reboot
4) switch mode shortcut: 
   - switch to mouse mode: ALT+M
     > ctrl+ <left>/<right>/<up>/<down> to move mouse arrow
     > <enter> for mouse left click
     > ctrl+ <enter> for youtube timeline jump
     > ctrl+ <enter> + <left>/<right> to drag in yt timeline
     > alt+ <up>/<down> for mouse wheel up/down
     > <+>/<-> for to increase/decrease move step size
   - switch to touchscreen mode: ALT+T
     > ctrl+ <left>/<right>/<up>/<down> to move touchpot
