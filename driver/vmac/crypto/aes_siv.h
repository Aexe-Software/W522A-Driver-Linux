
#ifndef AES_SIV_H
#define AES_SIV_H

int aes_siv_encrypt(const u8 *key, size_t key_len,
		    const u8 *pw, size_t pwlen,
		    size_t num_elem, const u8 *addr[], const size_t *len,
		    u8 *out);
int aes_siv_decrypt(const u8 *key, size_t key_len,
		    const u8 *iv_crypt, size_t iv_c_len,
		    size_t num_elem, const u8 *addr[], const size_t *len,
		    u8 *out);

#endif 
