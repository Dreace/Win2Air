#ifndef SENDER_CHACHA_H
#define SENDER_CHACHA_H
#include <stdint.h>
#include <stddef.h>
#include <gcrypt.h>
void chacha_close(gcry_cipher_hd_t);
gcry_cipher_hd_t chacha_open(const uint8_t *, size_t);
int chacha_encrypt(uint8_t *, uint8_t *, size_t, const void *, size_t, uint8_t *, size_t, uint8_t *, size_t, gcry_cipher_hd_t);
#endif
