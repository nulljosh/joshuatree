#ifndef VIRTIO_H
#define VIRTIO_H
/* virtio.c: QEMU's virtio devices (input, network, disk), or the Pi's stand-ins for them. main.c brings them up and
   polls input; the network stack reaches the card through nic_*, the FAT code the disk through vblk_*. */
struct input_event { unsigned short type, code; unsigned value; };   /* a Linux evdev event, what every input driver speaks */
void input_event(struct input_event e);   /* main.c: one key, button or pointer event, from any driver */
int virtio_input_init(void);
void virtio_input_poll(void);
int virtio_net_init(void);
void virtio_net_probe(void);
int virtio_blk_init(void);
void virtio_blk_probe(void);
int wifi_nic_up(void);   /* wifi.c: the Pi's network card is its Wi-Fi chip, up once the handshake installed the keys */
#endif
