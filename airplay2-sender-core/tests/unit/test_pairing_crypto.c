/* Compile the same tests beside each unmodified HomeKit implementation. */
#include "pair_homekit.c"
#ifdef _WIN32
#include <windows.h>
#endif
int pairing_crypto_vectors(void) {
#ifdef PAIRING_SECURITY_TESTS
  struct pair_cipher_context enc = {0}, dec = {0};
  uint8_t *wire = NULL, *decoded = NULL;
  size_t wire_size = 0, decoded_size = 0;
  uint8_t message[] = {1, 2, 3, 4};
  assert(encrypt(&wire, &wire_size, message, sizeof(message), &enc) ==
         sizeof(message));
  for (size_t split = 0; split < wire_size; ++split) {
    struct pair_cipher_context fragmented = {0};
    decoded = NULL;
    assert(decrypt(&decoded, &decoded_size, wire, split, &fragmented) == 0);
    assert(decoded_size == 0 && fragmented.decryption_counter == 0);
    free(decoded);
  }
  uint8_t *with_tail = malloc(wire_size + 1);
  assert(with_tail);
  memcpy(with_tail, wire, wire_size);
  with_tail[wire_size] = 1;
#ifdef _WIN32
  SYSTEM_INFO system;
  GetSystemInfo(&system);
  uint8_t *pages = VirtualAlloc(NULL, 2 * system.dwPageSize,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  DWORD previous_protection;
  assert(pages && wire_size + 1 < system.dwPageSize);
  assert(VirtualProtect(pages + system.dwPageSize, system.dwPageSize,
                        PAGE_NOACCESS, &previous_protection));
  uint8_t *guarded = pages + system.dwPageSize - wire_size - 1;
  memcpy(guarded, with_tail, wire_size + 1);
  free(with_tail);
  with_tail = guarded;
#endif
  assert(decrypt(&decoded, &decoded_size, with_tail, wire_size + 1, &dec) ==
         (ssize_t)wire_size);
  assert(decoded_size == sizeof(message) &&
         !memcmp(decoded, message, sizeof(message)) &&
         dec.decryption_counter == 1);
  free(decoded);
#ifdef _WIN32
  assert(VirtualFree(pages, 0, MEM_RELEASE));
#else
  free(with_tail);
#endif
  free(wire);
#endif
  uint8_t input[32], output[32], cipher[32], plain[32], tag[16],
      nonce[12] = {0};
  for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;
  assert(!hkdf_extract_expand(output, sizeof(output), input, sizeof(input),
                              "Pair-Setup-Encrypt-Salt",
                              "Pair-Setup-Encrypt-Info"));
  printf("hkdf 32 ");
  for (size_t i = 0; i < sizeof(output); ++i) printf("%02x", output[i]);
  putchar('\n');
  assert(!encrypt_chacha(cipher, input, sizeof(input), output, sizeof(output),
                         "ad", 2, tag, sizeof(tag), nonce));
  printf("chacha 48 ");
  for (size_t i = 0; i < sizeof(cipher); ++i) printf("%02x", cipher[i]);
  for (size_t i = 0; i < sizeof(tag); ++i) printf("%02x", tag[i]);
  putchar('\n');
  assert(!decrypt_chacha(plain, cipher, sizeof(cipher), output, sizeof(output),
                         "ad", 2, tag, sizeof(tag), nonce) &&
         !memcmp(input, plain, sizeof(input)));
  tag[0] ^= 1;
  assert(decrypt_chacha(plain, cipher, sizeof(cipher), output, sizeof(output),
                        "ad", 2, tag, sizeof(tag), nonce) < 0);
  pair_tlv_values_t *values = pair_tlv_new();
  assert(values);
  uint8_t fragmented[513];
  memset(fragmented, 0xa5, sizeof(fragmented));
  assert(!pair_tlv_add_value(values, TLVType_PublicKey, fragmented,
                             sizeof(fragmented)));
  uint8_t encoded[600];
  size_t size = sizeof(encoded);
  assert(!pair_tlv_format(values, encoded, &size));
  pair_tlv_free(values);
  values = pair_tlv_new();
  assert(values && !pair_tlv_parse(encoded, size, values));
  pair_tlv_t *item = pair_tlv_get_value(values, TLVType_PublicKey);
  assert(item && item->size == sizeof(fragmented) &&
         !memcmp(item->value, fragmented, sizeof(fragmented)));
  pair_tlv_free(values);
  values = pair_tlv_new();
  assert(pair_tlv_parse(encoded, size - 1, values) < 0);
  pair_tlv_free(values);
  puts("tlv fragmentation and truncation verified");
  return 0;
}
