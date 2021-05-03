Python Module for Controlling the Si4703 FM tuner Chip on a raspberry PI.

Available Functions:

initialize() - Initializes the module, enables the oscillator and unmutes the device.

goToChannel(channel) - Tune to the requested channel. Channel must be an integer representation of the desired station, so 92.7 becomes 927 and 105.3 becomes 1053 and so on.

getChannel() - Returns the integer representation of the currently tuned channel. (as in goToChannel)

seek(up) - Seeks in the requested direction. 1 = up, = Down.


To Install this module:

This module currently requires Python 3.7
execute: "sudo python3 setup.py install" to install this module. Then you may "import Si4703" in your scripts.


