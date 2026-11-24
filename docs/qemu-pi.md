# Running the Pi kernel in QEMU (raspi4b)

For measuring and checking the Pi build without a Pi: QEMU's `raspi4b`
machine runs the real kernel (Circle, the JIT generating real AArch64
code), and its `libinsn` plugin counts the instructions each core
executes. QEMU does not model timing, so it gives instruction counts, not
speed.

1. Build the QEMU variant of the kernel (no USB host or Ethernet, which
   QEMU's raspi4b lacks; the SD card on the second controller as "SD2:"):

       make -C app/circle G3_DEFINES=-DG3_QEMU_BOOT

   (touch app/circle/*.cpp first, and rebuild without the define after.)
2. An SD card image with the settings, the ROM and a CD image:

       tools/qemu/make_sd_image.py sd.img 1024 G3MACPI/G3MACPI.INI=g3macpi.ini \
           G3MACPI/ROM/ROM.BIN="Mac OS ROM" G3MACPI/CD/CD921.ISO=macos921.iso G3MACPI/LOGS/

   (8.3 names only: point `rom` and `image` in the .ini at them.)
3. QEMU 9.2 needs a one-line change to answer the firmware property tag
   0x00038041 (set an expander GPIO; Circle's SD driver sets the card's
   1.8 V supply with it on the Pi 4): in `hw/misc/bcm2835_property.c`,
   handle `RPI_FWREQ_SET_GPIO_STATE` with `resplen = 8;`. Without it the
   SD card does not start.
4. Run:

       qemu-system-aarch64 -M raspi4b -kernel kernel8-rpi4.img \
           -dtb bcm2711-rpi-400.dtb -serial file:serial.txt -display none \
           -drive file=sd.img,if=sd,format=raw -append bootlog=1 \
           -plugin tests/tcg/plugins/libinsn.so,inline=on -d plugin -D insns.txt

   The log (serial.txt) has the Mac's instruction count every 10 s;
   insns.txt the instructions per core at exit (core 0 runs the Mac).
