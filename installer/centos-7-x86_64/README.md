This directory contains steps and scripts for how to setup environment for
building FBOSS, build FBOSS binaries and install on a switch using CentOS 7
x86-64.

# 1. Building FBOSS

This section covers how to build FBOSS binaries and all its library
dependencies. It involves creating a Virtual Machine environment, building
FBOSS and then creating RPM package that contains FBOSS binaries and library
dependencies.

Requirement: ability to create Virtual Machine that has Internet access.

## 1.1 Preparation

- Download ISO: CentOS 7 x86_64 from: https://www.centos.org/download/
- Create a Virtual Machine(VM) using this CentOS ISO Image.
- The VM can be created using any virtualization software e.g. VMware Fusion or
  KVM etc. Refer 'Installing VM on VMware Fusion' section for additional
  details.
- Sample VM configuration:: Disk space: 128Gb, RAM: 16G, CPUs: 8
  More RAM/CPUS the merrier (faster builds).
- VM needs connectivity to Internet.
  - From command prompt, run 'ip addr list' to list interfaces.
  - Request DHCP address on interface: e.g. dhclient ens33 # dhclient -6 for IPv6
  - verify Inetnet connectivity by ping'ing known Internet IP.
    For example, ping internet.org # ping6 internet.org for IPv6

This VM will be the build environment.

## 1.2 Get FBOSS

- sudo yum install git
- git clone https://github.com/facebook/fboss.git fboss.git

## 1.3 Upgrade kernel

CentOS 7.0 comes with kernel 3.10. This upgrades the kernel to latest long term
support kernel (currently 4.4).

- cd fboss.git
- ./installer/centos-7-x86_64/upgrade-kernel.sh
- reboot # issue this command from VM console.

During reboot, choose the newly installed kernel (default would still be 3.10).
On boot, run uname -a to verify the version of the kernel.

If it boots successfully into new kernel, change the default to newly installed
kernel as below:

Check /etc/default/grub.

If it has GRUB_DEFAULT=<number>,
  - edit it to GRUB_DEFAULT=0.
  - grub2-mkconfig -o /boot/grub2/grub.cfg
  - reboot # verify if default changed.

else if it has GRUB_DEFAULT=saved,
  - grub2-editenv list
  - grub2-set-default 0
  - reboot # verify if default changed.

Reference: https://www.tecmint.com/install-upgrade-kernel-version-in-centos-7/

## 1.4 Install tools

Install tools needed for bulding FBOSS software.

- cd fboss.git
- ./installer/centos-7-x86_64/install-tools.sh

## 1.5 Build FBOSS binaries

- export PATH=/opt/rh/devtoolset-8/root/usr/bin/:$PATH
- cd fboss.git
- ./build/fbcode_builder/getdeps.py build fboss

The built FBOSS binaries will be available here:
/tmp/fbcode_builder_getdeps-ZhomeZ${USER}Zfboss.gitZbuildZfbcode_builder/installed/fboss/bin/{wedge_agent, bcm_test}

## 1.6 Build tips

- The FBOSS build step downloads and builds all FBOSS dependencies as listed in
  build/fbcode_builder/manifests/fboss (and recursively does the same for every
  listed dependency).
- The amount of time it takes to build depends on the number of vCPUS, memory
  etc. A 32 vcpu VM with 16G RAM and 512M swap space builds FBOSS in
  approximately 20 minutes.
- by default, fbcode_builder will use number of concurrent jobs for building to
  equal the number of cpu cores. That can be tweaked by setting --num-jobs
  e.g.:
      - cd fboss.git
      - ./build/fbcode_builder/getdeps.py build fboss --num-jobs 8
- If the build fails with below error
    c++: fatal error: Killed signal terminated program cc1plus

  check the swap utilization (run free -mhb), consider increasing the number of
  vcpus/memory/swap size etc. Or, if that is not possible, consider reducing
  the number of concurrent jobs using --num-jobs.

## 1.7 Building RPM packages

- cd fboss.git
- ./installer/centos-7-x86_64/build-rpm.sh

The built RPM package will contain the FBOSS binaries and all the dependent
libraries. The RPM would be available here:

$HOME/rpmbuild/RPMS/x86_64/fboss_bins-1-1.el7.centos.x86_64.rpm


# 2. Installing FBOSS

This section covers how to install FBOSS RPM package built using instructions
in previous section.

Requirements:
- access to hardware (switch) that can run FBOSS.
- The switch should have serial console access.
- CentOS VM configured in Building FBOSS section above.
- ability to copy files from the CentOS VM to the switch.

This section is written assuming that the switch does not have Internet
connectivity. However, if the switch has Internet connectivity, some of the
steps will be simpler - e.g. upgradeing kernel can do a direct 'yum install'
similar to what is described in "1.3 Upgrade kernel" for the VM.

## 2.1 Install CentOS 7.0

This should follow usual instructions for installing a Linux distribution on
x86_64 machine. In particular, install CentOS 7 x86_64 ISO from:
  https://www.centos.org/download/

As an example, refer "Installing CentOS 7 from bootable USB" section.

## 2.2 Upgrade kernel

This section assumes that the switch has no direct Internet connectivity. If
the switch has direct Internet connectivity, follow instructions similar to
"1.3 Upgrade kernel" to yum install directly.

### 2.2.1 Download new kernel rpms

From the CentOS VM configured in section 1. Building, download the kernel RPM.

- yum install yum-utils   # installs yumdownloader
- yumdownloader --enablerepo=elrepo-kernel kernel-lt    # this downloads an rpm to current directory

### 2.2.2 Install new kernel

- Copy the downloaded new kernel rpm from CentOS VM to the switch.
- rpm -ivh kernel-lt-4.4.181-1.el7.elrepo   # install the RPM.

Follow instructions in "1.3 Upgrade kernel" to reboot, verify and change the
grub boot order.

## 2.3 Install FBOSS RPM binary

- Copy the built FBOSS RPM built in Section "1.7 Building RPM packages" to the switch.
- Install the RPM
   rpm -ivh fboss_bins-1-1.el7.centos.x86_64.rpm
- The RPM will install binaries and dependent libraries in /etc/fboss_bins/
- export LD_LIBRARY_PATH=/etc/fboss_bins
- Run /etc/fboss_bins/{wedge_agent, bcm_test} etc. with desired arguments.

## 2.4 Install tips

### 2.4.1 Checking dependencies

Check dependencies using ldd.  All shared object dependencies should be
satisified, and dependencies should be picked from /etc/fboss_bins.

ldd /etc/fboss_bins/bcm_test # or wedge_agent

### 2.4.2 Checking install RPM, removing it

- rpm -qa | grep fboss    # check all installed RPMS.
- sudo rpm -e fboss_bins-1-1.el7.centos.x86_64  # remove installed RPM
- sudo rpm -ivh fboss_bins-1-1.el7.centos.x86_64.rpm # reinstall the RPM


# 3. Misc

This section details workflows using particular versions of software. The
build/install process has been successfully verified using these workflows and
should thus serve as handy reference.

## 3.1 Installing VM on VMware Fusion

These steps worked on a Macbook laptop running macOS Mojave version 10.14.4.

- Install VMware Fusion Professional Version 11.1.0 (13668589)
- Download CentOS-7-x86_64-DVD-1810.iso https://www.centos.org/download/
- Start VMware Fusion : File => New => drag and drop CentOS ISO on
   “Install from disc or image”.
- On macOS Mojave Version 10.14.4, this prompts “Could not open
  /dev/vmmon: Broken Pipe”. Fix by setting right security & privacy
  settings. Reference: http://communities.vmware.com/thread/600496
- Before beginning installation, click on VM settings and change
  hard disk size to at least 64G, CPUs: 2 (max the machine can reasonably
  support), Memory 2G (again, max possible).

## 3.2 Installing CentOS 7 from bootable USB

These steps worked on a Macbook laptop running macOS Mojave version 10.14.4.
These steps require a USB 2.0 or USB 3.0 of size >= 16GB.

### 3.2.1 Create a bootable USB

- Download ISO: CentOS 7 x86_64 from: https://www.centos.org/download/
- Convert downloaded .iso to .img (specifically, a UDIF read/write image) by
  running following from Macbook terminal:

    hdiutil convert -format UDRW -o CentOS-7-x86_64-DVD-1810.img CentOS-7-x86_64-DVD-1810.iso

- Insert USB 2.0 or USB 3.0 drive into the macbook.
- diskutil list  # find out the name of the USB disk just inserted e.g.  /dev/disk3
- diskutil unmountDisk <disk-name> # unmount the disk e.g. diskutil unmountDisk /dev/disk3
- time sudo dd if=CentOS-7-x86_64-DVD-1810.img.dmg of=/dev/disk3 bs=1m # this took over 25 minutes
  Note: the last step can take several minutes without reporting any progress.
  Took over 25 minutes on Macbook laptop with Interl i7 processor and 16G RAM.

Reference: http://www.myiphoneadventure.com/os-x/create-a-bootable-centos-usb-drive-with-a-mac-os-x

### 3.2.2 Use the bootable USB to install

- Power on the switch and connect to its serial console (BMC).
- Insert the bootable USB.
- Login as root using the default password.
- From the command prompt, run "/usr/local/bin/wedge_power.sh reset"
- Then, run "/usr/local/bin/sol.sh" to get serial console to the wedge to
  observe the wedge restart.
- During restart, hit 'escape' or 'delete' or 'shift delete' to get to the BIOS.
  Depending on the environment used to connect to console, one of the above key
  combinations should work.
- From BIOS, select to boot from USB.
  (note: if the configuration order is changed to boot from USB, don't forget
  to change it back to boot from hard disk after the install is complete.
  Alternatively, simply choose to boot from USB for the current boot).
- This will boot from USB and get to the Linux grub prompt.
- By default, Linux will attempt GUI install, to prevent it, at grub prompt:
    - edit line that has 'quite' in the end to add: inst.text inst.headless console=ttyS0,57600
    - hit 'control + x' to boot using above setting, this will do text install from the console.
      - Set the timezone, root password
      - Choose defaults for destination disk: "Use all of space", "LVM".
    - Note: post install step takes several minutes.
    - reboot
- If the boot order was changed, enter BIOS again to change it back to boot
  from hard disk first.

### 3.2.3 Configure Network

This assumes DHCP server is present in the network.

- ip addr list
- dhclient <interface-name> or dhclient -6 <interface-name> # for IPv4 and IPv6 respectively
- the above has to be run after every reboot, to make it persistent:
  - ip addr list    # find out the interface name
  - Edit /etc/sysconfig/network-scripts/ifcfg-<interface-name>
  - e.g. for /etc/sysconfig/network-scripts/ifcfg-enp2s0 if the interface name is enp2s0
  - Set ONBOOT=yes
