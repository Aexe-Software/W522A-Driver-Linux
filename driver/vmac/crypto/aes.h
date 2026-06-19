
#ifndef AES_H
#define AES_H

#define AES_BLOCK_SIZE 16

void * aes_encrypt_init(const u8 *key, size_t len);
int aes_encrypt(void *ctx, const u8 *plain, u8 *crypt);
void aes_encrypt_deinit(void *ctx);
void * aes_decrypt_init(const u8 *key, size_t len);
int aes_decrypt(void *ctx, const u8 *crypt, u8 *plain);
void aes_decrypt_deinit(void *ctx);

#endif 
