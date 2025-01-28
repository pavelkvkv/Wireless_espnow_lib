# Description

This is a library for a reliable transfer of not very large data on the ESP NOU protocol. It contains a number of opportunities that the original protocol does not have: 
- Request and write parameters (integers, blobs, strings) with flexible wrapping for your needs (look in examples/wireless_params.c)
- Read and write files in one-function manner, request file lists (look for io wrappers in examples/wireless_port.h)
- Unicast data feeding and receiving (look in examples/wireless_feed.c)
- Robust two-step pairing algorithm based on BSSID broadcasting with dual confirmation
- Easy to add your own logical data channels with custom functionality
- A decent low-level algorithm implementing retransmissions of lost packets based on counting missing indices, with low overhead and checksum verification.

# Speed and Latency

With default chunk size it reaches 10-50 ms send-to-ask latency on small packets (3-4 ESP-NOW messages) and speed about 10 KiB/s on file transfer.
There were no high-speed hacks to accelerate WiFi, the standard ESP-NOW speed was used.

# Creating an ESP-IDF component

- Create folder: Project_folder/components/Wireless (for example)
- git clone @thisrepo@ Project_folder/components/Wireless/wireless_lib_espnow
- Create CMakeLists.txt, include folder and other files, such as:
```
.
├── include
│   ├── wireless.h
│   └── wireless_port.h
├── wireless_lib_espnow
│   ├── include
│   │   ├── private.h
│   │   ├── w_files.h
│   │   ├── w_main.h
│   │   ├── w_param.h
│   │   └── w_user.h
│   ├── README.md
│   ├── w_channels.c
│   ├── w_connect.c
│   ├── w_files.c
│   ├── w_main.c
│   └── w_param.c
├── CMakeLists.txt
├── wireless_feed.c
├── wireless_params.c
└── wireless_port.c
```
 


