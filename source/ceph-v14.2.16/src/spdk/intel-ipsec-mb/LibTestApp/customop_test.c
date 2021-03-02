/*****************************************************************************
 Copyright (c) 2017-2018, Intel Corporation

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intel-ipsec-mb.h>

#include "customop_test.h"

#define DIM(_a)	(sizeof(_a) / sizeof(_a[0]))

#ifdef DEBUG
#ifdef _WIN32
#define TRACE(fmt, ...)	fprintf(stderr, "%s:%d "fmt, \
                                __FUNCTION__, __LINE__, __VA_ARGS__)
#else
#define TRACE(fmt, ...)	fprintf(stderr, "%s:%d "fmt, \
                                __func__, __LINE__, __VA_ARGS__)
#endif
#else
# define TRACE(fmt, ...)
#endif

struct cipher_attr_s {
        const char *name;
        JOB_CIPHER_MODE mode;
        unsigned key_len;
        unsigned iv_len;
};

struct auth_attr_s {
        const char *name;
        JOB_HASH_ALG hash;
        unsigned tag_len;
};

struct test_vec_s {
        uint8_t iv[16];
        uint8_t txt[64];
        uint8_t tag[32];
        uint8_t verify[32];

        DECLARE_ALIGNED(uint8_t enc_key[16*16], 64);
        DECLARE_ALIGNED(uint8_t dec_key[16*16], 64);
        uint8_t ipad[256];
        uint8_t opad[256];
        const struct cipher_attr_s *cipher;
        const struct auth_attr_s *auth;

        unsigned seq;
};

/*
 * addon cipher function
 */
static int
cipher_addon(struct JOB_AES_HMAC *job)
{
#ifdef DEBUG
        struct test_vec_s *node = job->user_data;
#endif

        TRACE("Seq:%u Cipher Addon cipher:%s auth:%s\n",
              node->seq, node->cipher->name, node->auth->name);

        if (job->cipher_direction == ENCRYPT)
                memset(job->dst, 1, job->msg_len_to_cipher_in_bytes);
        else
                memset(job->dst, 2, job->msg_len_to_cipher_in_bytes);

        return 0;	/* success */
}

/*
 * addon hash function
 */
static int
hash_addon(struct JOB_AES_HMAC *job)
{
#ifdef DEBUG
        struct test_vec_s *node = job->user_data;
#endif

        TRACE("Seq:%u Auth Addon cipher:%s auth:%s\n",
              node->seq, node->cipher->name, node->auth->name);

        memset(job->auth_tag_output, 3, job->auth_tag_output_len_in_bytes);
        return 0;	/* success */
}

/*
 * test cipher functions
 */
static const struct auth_attr_s auth_attr_tab[] = {
        { "SHA1", SHA1, 12 },
        { "SHA224", SHA_224, 14 },
        { "SHA256", SHA_256, 16 },
        { "SHA384", SHA_384, 24 },
        { "SHA512", SHA_512, 32 },
        { "MD5", MD5, 12 },
        { "CUSTOM_HASH", CUSTOM_HASH, 16 }
};

/*
 * test hash functions
 */
static const struct cipher_attr_s cipher_attr_tab[] = {
        { "CBC128", CBC, 16, 16 },
        { "CBC192", CBC, 24, 16 },
        { "CBC256", CBC, 32, 16 },
        { "CUSTOM_CIPHER", CUSTOM_CIPHER, 32, 12 },
        { "CTR128", CNTR, 16, 12 },
        { "CTR192", CNTR, 24, 12 },
        { "CTR256", CNTR, 32, 12 }
};

static int
job_check(const struct JOB_AES_HMAC *job)
{
#ifdef DEBUG
        struct test_vec_s *done = job->user_data;
#endif

        TRACE("done Seq:%u Cipher:%s Auth:%s\n",
              done->seq, done->cipher->name, done->auth->name);

        if (job->status != STS_COMPLETED) {
                TRACE("failed job status:%d\n", job->status);
                return -1;
        }
        if (job->cipher_mode == CUSTOM_CIPHER) {
                if (job->cipher_direction == ENCRYPT) {
                        unsigned i;

                        for (i = 0; i < job->msg_len_to_cipher_in_bytes; i++) {
                                if (job->dst[i] != 1) {
                                        TRACE("NG add-on encryption %u\n", i);
                                        return -1;
                                }
                        }
                        TRACE("Addon encryption passes Seq:%u\n", done->seq);
                } else {
                        unsigned i;

                        for (i = 0; i < job->msg_len_to_cipher_in_bytes; i++) {
                                if (job->dst[i] != 2) {
                                        TRACE("NG add-on decryption %u\n", i);
                                        return -1;
                                }
                        }
                        TRACE("Addon decryption passes Seq:%u\n", done->seq);
                }
        }

        if (job->hash_alg == CUSTOM_HASH) {
                unsigned i;

                for (i = 0; i < job->auth_tag_output_len_in_bytes; i++) {
                        if (job->auth_tag_output[i] != 3) {
                                TRACE("NG add-on hashing %u\n", i);
                                return -1;
                        }
                }
                TRACE("Addon hashing passes Seq:%u\n", done->seq);
        }
        return 0;
}


void
customop_test(struct MB_MGR *mgr)
{
        struct test_vec_s test_tab[DIM(cipher_attr_tab) * DIM(auth_attr_tab)];
        struct JOB_AES_HMAC *job;
        unsigned i, j, seq;
        int result = 0;

        for (i = 0, seq = 0; i < DIM(cipher_attr_tab); i++) {
                for (j = 0; j < DIM(auth_attr_tab); j++) {
                        assert(seq < DIM(test_tab));
                        test_tab[seq].seq = seq;
                        test_tab[seq].cipher = &cipher_attr_tab[i];
                        test_tab[seq].auth = &auth_attr_tab[j];
                        seq++;
                }
        }

        /* encryption */
        for (i = 0; i < seq; i++) {
                struct test_vec_s *node = &test_tab[i];

                while ((job = IMB_GET_NEXT_JOB(mgr)) == NULL) {
                        job = IMB_FLUSH_JOB(mgr);
                        result |= job_check(job);
                }

                job->cipher_func = cipher_addon;
                job->hash_func = hash_addon;

                job->aes_enc_key_expanded = node->enc_key;
                job->aes_dec_key_expanded = node->dec_key;
                job->aes_key_len_in_bytes = node->cipher->key_len;
                job->src = node->txt;
                job->dst = node->txt;
                job->cipher_start_src_offset_in_bytes = 16;
                job->msg_len_to_cipher_in_bytes = sizeof(node->txt);
                job->hash_start_src_offset_in_bytes = 0;
                job->msg_len_to_hash_in_bytes =
                        sizeof(node->txt) + sizeof(node->iv);
                job->iv = node->iv;
                job->iv_len_in_bytes = node->cipher->iv_len;
                job->auth_tag_output = node->tag;
                job->auth_tag_output_len_in_bytes = node->auth->tag_len;

                job->u.HMAC._hashed_auth_key_xor_ipad = node->ipad;
                job->u.HMAC._hashed_auth_key_xor_opad = node->opad;
                job->cipher_mode = node->cipher->mode;
                job->cipher_direction = ENCRYPT;
                job->chain_order = CIPHER_HASH;
                job->hash_alg = node->auth->hash;
                job->user_data = node;

                job = IMB_SUBMIT_JOB(mgr);
                while (job) {
                        result |= job_check(job);
                        job = IMB_GET_COMPLETED_JOB(mgr);
                }
        }

        while ((job = IMB_FLUSH_JOB(mgr)) != NULL)
                result |= job_check(job);

        /* decryption */
        for (i = 0; i < seq; i++) {
                struct test_vec_s *node = &test_tab[i];

                while ((job = IMB_GET_NEXT_JOB(mgr)) == NULL) {
                        job = IMB_FLUSH_JOB(mgr);
                        result |= job_check(job);
                }

                job->cipher_func = cipher_addon;
                job->hash_func = hash_addon;

                job->aes_enc_key_expanded = node->enc_key;
                job->aes_dec_key_expanded = node->dec_key;
                job->aes_key_len_in_bytes = node->cipher->key_len;
                job->src = node->txt;
                job->dst = node->txt;
                job->cipher_start_src_offset_in_bytes = 16;
                job->msg_len_to_cipher_in_bytes = sizeof(node->txt);
                job->hash_start_src_offset_in_bytes = 0;
                job->msg_len_to_hash_in_bytes =
                        sizeof(node->txt) + sizeof(node->iv);
                job->iv = node->iv;
                job->iv_len_in_bytes = node->cipher->iv_len;
                job->auth_tag_output = node->tag;
                job->auth_tag_output_len_in_bytes = node->auth->tag_len;

                job->u.HMAC._hashed_auth_key_xor_ipad = node->ipad;
                job->u.HMAC._hashed_auth_key_xor_opad = node->opad;
                job->cipher_mode = node->cipher->mode;
                job->cipher_direction = DECRYPT;
                job->chain_order = HASH_CIPHER;
                job->hash_alg = node->auth->hash;
                job->user_data = node;

                job = IMB_SUBMIT_JOB(mgr);
                while (job) {
                        result |= job_check(job);
                        job = IMB_GET_COMPLETED_JOB(mgr);
                }
        }

        while ((job = IMB_FLUSH_JOB(mgr)) != NULL)
                result |= job_check(job);

        if (result)
                fprintf(stdout, "Custom cipher/auth test failed!\n");
        else
                fprintf(stdout, "Custom cipher/auth test passed\n");
}
