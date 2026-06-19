
#ifndef SHA256_I_H
#define SHA256_I_H

#define SHA256_BLOCK_SIZE 64

struct aml_sha256_state {
	u64 length;
	u32 state[8], curlen;
	u8 buf[SHA256_BLOCK_SIZE];
};

void aml_sha256_init(struct aml_sha256_state *md);
int aml_sha256_process(struct aml_sha256_state *md, const unsigned char *in,
		   unsigned long inlen);
int aml_sha256_done(struct aml_sha256_state *md, unsigned char *out);

#endif 
