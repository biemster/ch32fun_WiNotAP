#!/usr/bin/env python
"""
requires pyusb, which should be pippable
SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="d035", MODE="666"
sudo udevadm control --reload-rules && sudo udevadm trigger
"""
import os
import argparse
import usb.core
import usb.util
import socket
import struct
import threading
from time import sleep

ETH_IFACE_NAME      = "enp1s0"
MAC_PREFIX          = [0xc6, 0x32]
ETH_P_ALL           = 3

CH_USB_VENDOR_ID    = 0x1209    # VID
CH_USB_PRODUCT_ID   = 0xd035    # PID
CH_USB_INTERFACE    = 0         # interface number
CH_USB_EP_IN        = 0x85      # FRAME_IN endpoint
CH_USB_EP_OUT       = 0x06      # FRAME_OUT endpoint
CH_USB_PACKET_SIZE  = 2048      # packet size, pyUSB does reassembly of 64 byte frames
CH_USB_TIMEOUT_MS   = 100       # timeout for USB operations

CH_CMD_REBOOT       = 0xa2
CH_STR_REBOOT       = (CH_CMD_REBOOT, 0x01, 0x00, 0x01)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bootloader', help='Reboot to bootloader', action='store_true')
    args = parser.parse_args()

    usb_dev = usb.core.find(idVendor=CH_USB_VENDOR_ID, idProduct=CH_USB_PRODUCT_ID)

    if usb_dev is None:
        print("MCU not found")
        exit(0)

    if args.bootloader:
        print('rebooting to bootloader')
        bootloader(usb_dev)
        sleep(.3)
    else:
        router(usb_dev)
    
    print('done')

def bootloader(usb_dev):
    usb_dev.write(CH_USB_EP_OUT, CH_STR_REBOOT)

def router(usb_dev):
    raw_sock = get_raw_socket(ETH_IFACE_NAME)

    stop_event = threading.Event()
    net_thread = threading.Thread(target=network_listener_thread, args=(usb_dev, raw_sock, stop_event))
    net_thread.daemon = True
    net_thread.start()

    print("Starting USB -> Network Bridge...")
    try:
        while True:
            try:
                data = usb_dev.read(CH_USB_EP_IN, CH_USB_PACKET_SIZE, timeout=CH_USB_TIMEOUT_MS)
                process_usb_data(raw_sock, data)
            except usb.core.USBError as e:
                if e.errno == 110:
                    # timeout, retry
                    continue
                elif e.errno == 19:
                    print("Disconnected")
                    break
                else:
                    print(f'USB Error: {str(e)}')
                    break
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        stop_event.set()
        raw_sock.close()


def process_usb_data(raw_sock, data):
    print(f"[USB -> ETH {len(data)} bytes]")
    # hexdump(data)
    raw_sock.send(bytes(data))

def process_eth_data(usb_dev, data):
    dest_mac = data[0:6]
    if dest_mac[:2] == bytes(MAC_PREFIX) or dest_mac == b'\xff\xff\xff\xff\xff\xff':
        print(f"[ETH -> USB {len(data)} bytes]")
        # hexdump(data)
        usb_dev.write(CH_USB_EP_OUT, len(data).to_bytes(2, 'little') + data, timeout=CH_USB_TIMEOUT_MS)

def hexdump(data, length=16):
    if not data:
        print("(No data)")
        return

    for i in range(0, len(data), length):
        chunk = data[i : i + length]

        hex_bytes = [f"{b:02x}" for b in chunk]
        hex_str = " ".join(hex_bytes)

        padding_width = length * 3 - 1
        hex_str = hex_str.ljust(padding_width)

        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)

        print(f"{i:08x}: {hex_str}  |{ascii_str}|")

def get_raw_socket(iface_name):
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        s.bind((iface_name, 0))
        return s
    except PermissionError:
        print(f"Error: You need root/sudo privileges to open a raw socket on {iface_name}.")
        exit(1)

def network_listener_thread(usb_dev, raw_sock, stop_event):
    print(f"Starting Network -> USB Bridge on {ETH_IFACE_NAME}...")

    while not stop_event.is_set():
        try:
            frame, addr = raw_sock.recvfrom(65535)
            process_eth_data(usb_dev, frame)
        except socket.timeout:
            continue
        except usb.core.USBError as e:
            print(f"USB Write Error: {e}")
            if e.errno == 19: # Device unplugged
                stop_event.set()
                break
        except Exception as e:
            print(f"Net->USB Error: {e}")


if __name__ == '__main__':
    main()
