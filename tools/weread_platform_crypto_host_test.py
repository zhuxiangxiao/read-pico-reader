#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Run production platform AES/MD5 glue against independent system primitives and the protocol vectors."""
from pathlib import Path
import platform, subprocess, tempfile, shlex

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='weread-platform-crypto-') as folder:
    path=Path(folder);(path/'aes').mkdir()
    (path/'aes/esp_aes.h').write_text('''
#include <stddef.h>
#include <stdint.h>
#define ESP_AES_DECRYPT 0
typedef struct {unsigned char key[16];} esp_aes_context;
void esp_aes_init(esp_aes_context*);
void esp_aes_free(esp_aes_context*);
int esp_aes_setkey(esp_aes_context*,const unsigned char*,unsigned);
int esp_aes_crypt_cbc(esp_aes_context*,int,size_t,unsigned char*,const unsigned char*,unsigned char*);
''')
    apple=platform.system()=='Darwin'
    (path/'esp_rom_md5.h').write_text(('''#include <CommonCrypto/CommonDigest.h>
typedef CC_MD5_CTX md5_context_t;
''' if apple else '''#include <openssl/md5.h>
typedef MD5_CTX md5_context_t;
''')+'''
#include <stdint.h>
void esp_rom_md5_init(md5_context_t*);
void esp_rom_md5_update(md5_context_t*,const void*,uint32_t);
void esp_rom_md5_final(uint8_t*,md5_context_t*);
''')
    common='''
#include "aes/esp_aes.h"
#include "esp_rom_md5.h"
#include <assert.h>
#include <string.h>
void esp_aes_init(esp_aes_context*c){memset(c,0,sizeof(*c));}
void esp_aes_free(esp_aes_context*c){memset(c,0,sizeof(*c));}
int esp_aes_setkey(esp_aes_context*c,const unsigned char*k,unsigned bits){if(bits!=128)return -1;memcpy(c->key,k,16);return 0;}
'''
    if apple:
        common+='''
#include <CommonCrypto/CommonCryptor.h>
int esp_aes_crypt_cbc(esp_aes_context*c,int mode,size_t bytes,unsigned char*iv,const unsigned char*in,unsigned char*out){
 assert(mode==ESP_AES_DECRYPT);size_t used=0;
 int err=CCCrypt(kCCDecrypt,kCCAlgorithmAES128,0,c->key,16,iv,in,bytes,out,bytes,&used);
 if(!err){assert(used==bytes);memcpy(iv,in+bytes-16,16);}return err;
}
void esp_rom_md5_init(md5_context_t*c){assert(CC_MD5_Init(c));}
void esp_rom_md5_update(md5_context_t*c,const void*p,uint32_t n){assert(CC_MD5_Update(c,p,n));}
void esp_rom_md5_final(uint8_t*out,md5_context_t*c){assert(CC_MD5_Final(out,c));}
'''
        libs=[]
    else:
        common+='''
#include <openssl/aes.h>
int esp_aes_crypt_cbc(esp_aes_context*c,int mode,size_t bytes,unsigned char*iv,const unsigned char*in,unsigned char*out){
 assert(mode==ESP_AES_DECRYPT);AES_KEY key;if(AES_set_decrypt_key(c->key,128,&key))return -1;
 AES_cbc_encrypt(in,out,bytes,&key,iv,AES_DECRYPT);return 0;
}
void esp_rom_md5_init(md5_context_t*c){assert(MD5_Init(c));}
void esp_rom_md5_update(md5_context_t*c,const void*p,uint32_t n){assert(MD5_Update(c,p,n));}
void esp_rom_md5_final(uint8_t*out,md5_context_t*c){assert(MD5_Final(out,c));}
'''
        libs=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','openssl'],text=True))
    (path/'platform.c').write_text(common)
    subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-DWEREAD_USE_PLATFORM_CRYPTO','-pthread',
        '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-fsanitize=address,undefined',
        '-I'+str(path),'-Imain','-Imain/book','-Itools/weread_stubs',
        'tools/test_weread_crypto.c','main/book/book_weread_crypto.c',str(path/'platform.c'),
        '-lz',*libs,'-o',str(path/'test')],cwd=root,check=True)
    subprocess.run([str(path/'test')],cwd=root,check=True)
print('PASS: production platform AES-CBC/ROM-MD5 glue agrees with independent system primitives on all protocol vectors')
