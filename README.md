# twofing

twofing is a daemon which runs in the background and recognizes two-finger gestures performed on a touchscreen and converts them into mouse and keyboard events. This way, such gestures can be used in almost all existing applications (even ones where you wouldn’t expect it, like Wine applications) without having to modify them.

# Installation

```
sudo apt-get install \
  build-essential \
  libx11-dev \
  libxtst-dev \
  libxi-dev \
  x11proto-randr-dev \
  libxrandr-dev \
  xserver-xorg-input-evdev \
  xserver-xorg-input-evdev-dev
make
sudo make install
```

Create a x11 conf file and add the following section to it (replace DEVICENAME with your device name, you can get it with `xinput list`.

```
Section "InputClass"
  Identifier "calibration"
  Driver "evdev"
  # replace DEVICENAME in the following line with your device name
  MatchProduct "DEVICENAME"

  Option "EmulateThirdButton" "1"
  Option "EmulateThirdButtonTimeout" "750"
  Option "EmulateThirdButtonMoveThreshold" "30"
EndSection
```
## Special install script for Argonaut M7
An install script for the Argonaut M7 (courtesy of Mikhail Grushinskiy) can be found here: https://github.com/bareboat-necessities/my-bareboat/blob/master/twofing/rpi_twofing_install.sh
