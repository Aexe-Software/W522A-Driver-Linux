
#include "aml_crypto_wrap.h"

#include "aes.h"
#include "aes_wrap.h"

int aes_ctr_encrypt(const u8 *key, size_t key_len, const u8 *nonce,
		    u8 *data, size_t data_len)
{
	void *ctx;
	size_t j, left = data_len;
	int i;
	u8 *pos = data;
	u8 counter[AES_BLOCK_SIZE], buf[AES_BLOCK_SIZE];

	ctx = aes_encrypt_init(key, key_len);
	if (ctx == NULL)
		return -1;
	os_memcpy(counter, nonce, AES_BLOCK_SIZE);

	while (left > 0) {
		size_t len;

		aes_encrypt(ctx, counter, buf);

		len = (left < AES_BLOCK_SIZE) ? left : AES_BLOCK_SIZE;
		for (j = 0; j < len; j++)
			pos[j] ^= buf[j];
		pos += len;
		left -= len;

		for (i = AES_BLOCK_SIZE - 1; i >= 0; i--) {
			counter[i]++;
			if (counter[i])
				break;
		}
	}
	aes_encrypt_deinit(ctx);
	return 0;
}

int aes_128_ctr_encrypt(const u8 *key, const u8 *nonce,
			u8 *data, size_t data_len)
{
	return aes_ctr_encrypt(key, 16, nonce, data, data_len);
}
