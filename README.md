#
Custom kernel source made to Galaxy A01 Core/Galaxy J2 Prime ( Linux 4.14.141 )

## Status
* Galaxy A01 Core: All functions work
* Galaxy J2 Prime: 
  * Unknown status
  * No Camera drivers ( back + front )
  * No Connectivity drivers ( wifi + bt )
  * No ALS/PS drivers
  * No Sensor drivers
  * No sm5701_charger driver

## Kernel features:
* Based on A013FPUU1ATJ4
* Selinux permissive by default
* Disabled Samsung MTP
* grandpplte: New `zinitix bt541` touch driver from `msm8916-mainline/linux`
* grandpplte: New mali midgard r26p0 GPU driver

