#!/usr/bin/env python3
"""Drive the FH1 from the keyboard over the serial console (the fallback when Wi-Fi is not up).

  keys.py [/dev/ttyUSB0]

Keys: arrows or w a s d, j = A, k = B, u = X, i = Y, Enter = Start, Space = Select, m = Menu.
Type ! to send a console command line, e.g. "!bl 80" or "!wifi <ssid> <password>". Ctrl-C quits.
The device's log is shown as it arrives. The port is opened without touching DTR/RTS, so it does not
reset the board.
"""
import os
import select
import sys
import termios
import tty

import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0
    s.dtr = s.rts = False
    s.open()
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    print(f"{port}: keys go to the FH1; ! starts a command; Ctrl-C quits\r")
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd, s.fileno()], [], [], 0.1)
            if s.fileno() in r:
                os.write(sys.stdout.fileno(), s.read(4096).replace(b"\n", b"\r\n"))
            if fd in r:
                c = os.read(fd, 8)
                if c == b"\x03":
                    break
                if c == b"!":
                    termios.tcsetattr(fd, termios.TCSADRAIN, old)
                    line = input("\r\ncommand: !")
                    tty.setraw(fd)
                    s.write(b"!" + line.encode() + b"\n")
                else:
                    s.write(c)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        print()


if __name__ == "__main__":
    main()
