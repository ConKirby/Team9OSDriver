# Assignment: Develop a Linux Kernel Device Driver Module

**Deadline:** 19th March 2026 11:59pm
**Submit process:** Upload zip file to Brightspace with a GitHub link

## Overview

Your assignment is to create a Linux loadable kernel module (LKM) that can be inserted into the running kernel using the `insmod` command and a user space application that demonstrates use of the module.

## Requirements

### Kernel Module

The LKM must at a minimum implement a simple character device driver interface and must implement the following functions:

- `open`
- `close`
- `read`
- `write`
- `ioctl`

The `read` and `write` functions should block if information or space is not available. The module should ideally create one or more `/proc` files to allow statistics to be queried.

### User Space Application

The user space application should be multi-process and/or multi-threaded and should demonstrate blocking calls to the LKM.

## Suggested Functionality

The actual functionality of the driver is left open to the team. Some suggestions are:

- A simple loopback device that echos back data
- A driver that does a simple transform such as ROT13
- Implement a FIFO of fixed size between two user space processes
- **Extra credit:** The driver could even be for an actual piece of hardware such as a keyboard or mouse

## Additional Info

No skeleton or sample files are provided. It is up to teams and individual students to find examples online and adapt these. As always, every student will be interviewed to ensure that they understand all the code in the project.

A Git repository should be set up by the end of Week 7 of Block 1.3. The repository should contain an initial commit with some initial files otherwise you will be docked marks. Your repositories and contributions will also be monitored throughout the duration of the project.

> **NOTE:** It is not easy to create and load a LKM into Windows WSL2 without building a custom kernel. For this reason, WSL is not recommended. Instead, a module can be built for a virtual Linux instance on your laptop, in the ISE cloud, for a Raspberry Pi, or for a desktop PC.

> **NOTE:** While VS Code can be used for editing code, it is not possible to run LKMs under VS Code. Building and loading modules will have to be done from the command line.
