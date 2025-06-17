# Iris Video Driver

This repository contains the source code of V4L2 Iris Video driver for VPUs.
Required to use IRIS hardware on Qualcomm Snapdragon targets.

Iris is a multi pipe based hardware that offloads video stream decoding 
from the application processor (AP). It supports H.264 decoding. The AP 
communicates with hardware through a well defined protocol, called as 
host firmware interface (HFI), which provides fine-grained and 
asynchronous control over individual hardware features.

Iris Video is a V4L2 complaint video driver with M2M and STREAMING capability.

This driver comes with below features:
- Centralized resource management.
- Centralized management of core and instance states.
- Defines platform specific capabilities and features. As a results, it 
  provides a single point of control to enable/disable a given feature 
  depending on specific platform capabilities.
- Handles various video recommended sequences, like DRC, Drain, Seek, 
  EOS.
- Implements asynchronous communication with hardware to achieve better 
  experience in low latency usecases.
- Output and capture planes are controlled independently. Thereby
  providing a way to reconfigure individual plane.
- Native hardware support of LAST flag which is mandatory to align with 
  port reconfiguration and DRAIN sequence as per V4L guidelines.

# Getting in Contact

Problems specific to the Iris Video driver can be reported in the Issues
section of this repository.

# License
This driver is released under the GPL-2.0 license. See LICENSE.txt for details.
