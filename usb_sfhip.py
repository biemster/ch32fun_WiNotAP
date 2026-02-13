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
from binascii import unhexlify
import threading
import subprocess
from time import sleep

ETH_IFACE_NAME      = "enp1s0"
HOST_MAC            = bytes([b for b in unhexlify(open(f'/sys/class/net/{ETH_IFACE_NAME}/address').read().strip().replace(':',''))])
HOST_IP             = b'\x00' *4
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

arp_table = {}
configured_routes = set()
arp_lock = threading.Lock()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--bootloader', help='Reboot to bootloader', action='store_true')
    parser.add_argument('-i', '--interface', help='Interface to bridge USB with')
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
        if args.interface:
            ETH_IFACE_NAME = args.interface
            HOST_MAC = bytes([b for b in unhexlify(open(f'/sys/class/net/{ETH_IFACE_NAME}/address').read().strip().replace(':',''))])
        router(usb_dev)
    
    print('done')

def bootloader(usb_dev):
    usb_dev.write(CH_USB_EP_OUT, CH_STR_REBOOT)

def router(usb_dev):
    sock_ext = get_raw_socket(ETH_IFACE_NAME)
    sock_lo = get_raw_socket('lo')

    stop_event = threading.Event()
    thread_ext = threading.Thread(target=network_listener_thread, args=(usb_dev, sock_ext, ETH_IFACE_NAME, stop_event), daemon=True)
    thread_lo = threading.Thread(target=network_listener_thread, args=(usb_dev, sock_lo, 'lo', stop_event), daemon=True)
    thread_ext.start()
    thread_lo.start()

    print("Starting USB -> Network Bridge...")
    try:
        while True:
            try:
                data = usb_dev.read(CH_USB_EP_IN, CH_USB_PACKET_SIZE, timeout=CH_USB_TIMEOUT_MS)
                process_usb_data(sock_ext, sock_lo, data)
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
        sock_ext.close()


def process_usb_data(sock_ext, sock_lo, data):
    # hexdump(data)
    frame = bytes(data)
    dest_mac = frame[:6]
    ethertype = frame[12:14]

    if ethertype == b'\x08\x06': # ARP
        parse_arp_packet(frame)

    if dest_mac == HOST_MAC:
        print(f"[USB -> LO  {len(frame)} bytes, {':'.join([format(x,'02x') for x in frame[6:12]])} -> {':'.join([format(x,'02x') for x in frame[:6]])}]")
        if isinstance(frame, bytes): frame = bytearray(frame)
        frame[:12] = b'\x00' *12
        sock_lo.send(frame)
    else:
        print(f"[USB -> ETH {len(frame)} bytes, {':'.join([format(x,'02x') for x in frame[6:12]])} -> {':'.join([format(x,'02x') for x in frame[:6]])}]")
        sock_ext.send(frame)

def process_eth_data(usb_dev, data):
    global HOST_IP
    dest_mac = data[0:6]
    ethertype = data[12:14]

    if ethertype != b'\x08\x00': # IPv4
        return

    if dest_mac[:2] == bytes(MAC_PREFIX) or dest_mac == b'\xff\xff\xff\xff\xff\xff':
        if data[36:38] == b'\x00\x44' and data[23] == 17: # 17 = UDP
            parse_dhcp_ack(data)

        # hexdump(data)
        print(f"[ETH -> USB {len(data)} bytes, {':'.join([format(x,'02x') for x in data[6:12]])} -> {':'.join([format(x,'02x') for x in data[:6]])}]")
        usb_dev.write(CH_USB_EP_OUT, len(data).to_bytes(2, 'little') + data, timeout=CH_USB_TIMEOUT_MS)
    elif data[:12] == b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00':
        # from host over lo
        dest_ip = bytes(data[30:34])

        if dest_ip == HOST_IP or dest_ip[0] == 127:
            return

        with arp_lock:
            target_mac = arp_table.get(dest_ip)

        if target_mac:
            # We know the MAC, rewrite header and send
            if isinstance(data, bytes): data = bytearray(data)
            data[0:6] = target_mac
            data[6:12] = HOST_MAC
            # hexdump(data)
            print(f"[LO  -> USB {len(data)} bytes, {':'.join([format(x,'02x') for x in data[6:12]])} -> {':'.join([format(x,'02x') for x in data[:6]])}]")
            usb_dev.write(CH_USB_EP_OUT, len(data).to_bytes(2, 'little') + data, timeout=CH_USB_TIMEOUT_MS)
        else:
            print(f"[?] Unknown destination {socket.inet_ntoa(dest_ip)}. Probing...")
            send_arp_request(usb_dev, dest_ip)
    elif HOST_IP == b'\x00\x00\x00\x00' and dest_mac == HOST_MAC:
        HOST_IP = data[30:34]
        print(f"[*] Detected Host IP: {socket.inet_ntoa(HOST_IP)}")

def parse_arp_packet(frame):
    sender_mac = frame[22:28]
    sender_ip = frame[28:32]
    update_arp_table(sender_ip, sender_mac)

def parse_dhcp_ack(frame):
    if frame[42] != 2: return 
    ip = frame[58:62]
    mac = frame[70:76]
    if ip != b'\x00\x00\x00\x00':
         update_arp_table(ip, mac)

def add_route(ip_str):
    try:
        cmd = ["ip", "route", "add", ip_str, "dev", "lo"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"[+] Added route: {ip_str} dev lo")
    except subprocess.CalledProcessError:
        # Usually happens if route already exists
        pass

def update_arp_table(ip_bytes, mac_bytes):
    with arp_lock:
        if ip_bytes not in arp_table:
            ip_str = socket.inet_ntoa(ip_bytes)
            print(f"[+] Learned: IP={ip_str} is MAC={mac_bytes.hex(':')}")
            arp_table[ip_bytes] = mac_bytes
            
            if ip_bytes not in configured_routes:
                add_route(ip_str)
                configured_routes.add(ip_bytes)

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

def send_arp_request(usb_dev, target_ip):
    eth_hdr = b'\xff\xff\xff\xff\xff\xff' + HOST_MAC + b'\x08\x06'
    arp_hdr = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1)
    sender_ip = b'\x00\x00\x00\x00' 
    target_mac_empty = b'\x00' * 6
    
    arp_payload = HOST_MAC + sender_ip + target_mac_empty + target_ip
    packet = eth_hdr + arp_hdr + arp_payload
    
    if len(packet) < 60:
        packet += b'\x00' * (60 - len(packet))

    usb_dev.write(CH_USB_EP_OUT, len(packet).to_bytes(2, 'little') + packet, timeout=CH_USB_TIMEOUT_MS)

def get_raw_socket(iface_name):
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        s.bind((iface_name, 0))
        return s
    except PermissionError:
        print(f"Error: You need root/sudo privileges to open a raw socket on {iface_name}.")
        exit(1)

def network_listener_thread(usb_dev, sock, iface, stop_event):
    print(f"Starting Network -> USB Bridge on {iface}...")

    while not stop_event.is_set():
        try:
            frame, _ = sock.recvfrom(65535)
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
