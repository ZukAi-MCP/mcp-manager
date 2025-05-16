/*
   Based on https://github.com/jackjack-jj/jeeq, GPLv3.
   Specifically designed to use Smileycoin key pairs for encoding/decoding so
   this is not as general as jeeq.py
   All the math here is explained pretty well on this wikipedia page:
   https://en.wikipedia.org/wiki/ElGamal_encryption
 */
#include <string.h>
#include <stdbool.h>
#include <boost/endian/conversion.hpp>

#include <vector>
#include <stdexcept>

#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/err.h>

#include "key.h"
#include "util.h"
#include "jeeq.h"

#define PRIVHEADER_LEN          9
#define PUBHEADER_LEN           7
#define PRIVKEY_LEN             32
#define COMPR_PUBKEY_LEN        33
#define UNCOMPR_PUBKEY_LEN      65
#define CHUNK_SIZE              32
#define VERSION                 0x00
#define GX_HEX                  "79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"
#define GY_HEX                  "483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8"
#define GORDER_HEX              "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141"
#define GCOFACTOR_HEX           "01"

using namespace boost::endian;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
/* ignore negative */
static int bn2binpad(const BIGNUM *a, unsigned char *to, int tolen)
{
    int n;
    size_t i, lasti, j, atop, mask;
    BN_ULONG l;

    /*
     * In case |a| is fixed-top, BN_num_bytes can return bogus length,
     * but it's assumed that fixed-top inputs ought to be "nominated"
     * even for padded output, so it works out...
     */
    n = BN_num_bytes(a);
    if (tolen == -1) {
        tolen = n;
    } else if (tolen < n) {     /* uncommon/unlike case */
        BIGNUM temp = *a;

        bn_correct_top(&temp);
        n = BN_num_bytes(&temp);
        if (tolen < n)
            return -1;
    }

    /* Swipe through whole available data and don't give away padded zero. */
    atop = a->dmax * BN_BYTES;
    if (atop == 0) {
        OPENSSL_cleanse(to, tolen);
        return tolen;
    }

    lasti = atop - 1;
    atop = a->top * BN_BYTES;
    for (i = 0, j = 0, to += tolen; j < (size_t)tolen; j++) {
        l = a->d[i / BN_BYTES];
        mask = 0 - ((j - atop) >> (8 * sizeof(i) - 1));
        *--to = (unsigned char)(l >> (8 * (i % BN_BYTES)) & mask);
        i += (i - lasti) >> (8 * sizeof(i) - 1); /* stay on last limb */
    }

    return tolen;
}

int BN_bn2binpad(const BIGNUM *a, unsigned char *to, int tolen)
{
    if (tolen < 0)
        return -1;
    return bn2binpad(a, to, tolen);
}

/*  Obviously not  */
void OPENSSL_clear_free(void *str, size_t num)
{
    memset(str, '\0', sizeof(char) * num);
    free(str);
}

static void *OPENSSL_zalloc(size_t num)
{
    void *ret = OPENSSL_malloc(num);

    if (ret != NULL)
        memset(ret, 0, num);
    return ret;
}

#endif
/* return:  secp256k1 curve with the bitcoin generator, order and cofactor
 * in:      ctx, bignum context
 */
static EC_GROUP *init_curve(BN_CTX *ctx)
{
    EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_secp256k1);

    EC_POINT *generator = EC_POINT_new(group);

    BIGNUM *Gx = BN_new();
    BIGNUM *Gy = BN_new();
    BIGNUM *order = BN_new();
    BIGNUM *cofactor = BN_new();

    BN_hex2bn(&Gx, GX_HEX);
    BN_hex2bn(&Gy, GY_HEX);

    BN_hex2bn(&order, GORDER_HEX);
    BN_hex2bn(&cofactor, GCOFACTOR_HEX);

    EC_POINT_set_affine_coordinates_GFp(group, generator, Gx, Gy, ctx);
    EC_GROUP_set_generator(group, generator, order, cofactor);

    BN_free(Gx);
    BN_free(Gy);
    BN_free(cofactor);
    BN_free(order);

    EC_POINT_free(generator);

    return group;
}

/* return: 1
 * out: m, pointer to message that will be prefaced with the private header
 * in:  nmsg, pointer to the message
 *      msg_length, length of the message
 */
static int write_private_header(uint8_t *m, const uint8_t *nmsg, const uint32_t msg_length)
{
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(nmsg, msg_length, hash);

    m[0] = VERSION;
    m[1] = 0x00;
    m[2] = 0x06;
    *(uint32_t*)&m[3] = native_to_big(msg_length);
    m[7] = hash[0];
    m[8] = hash[1];

    return 1;
}

/* return: 1
 * out:     enc, the encrypted string to preface the public header with
 * in:      pub, pointer to raw pubkey,
 *          is_compressed, whether the pubkey is compressed NEEDED
 */
static int write_public_header(uint8_t *enc, const uint8_t *pub)
{
    uint8_t hash[SHA256_DIGEST_LENGTH];
    size_t pubkey_length = (pub[0] == 0x04) ? UNCOMPR_PUBKEY_LEN : COMPR_PUBKEY_LEN;

    SHA256(pub, pubkey_length, hash);

    enc[0] = 0x6a;
    enc[1] = 0x6a;
    enc[2] = VERSION;
    enc[3] = 0x00;
    enc[4] = 0x02;
    enc[5] = hash[0];
    enc[6] = hash[1];

    return 1;
}

/* return:  1 on success, 0 otherwise
 * out:     message_length, read message length
 * in:      dec, decrypted string
 */
static int read_private_header(size_t *message_length, const uint8_t *dec)
{
    if (dec[0] != VERSION) return 0;
    if (dec[1] != 0x00) return 0;
    if (dec[2] != 0x06) return 0;

    uint32_t msg_len = big_to_native(*(uint32_t*)&dec[3]);

    /* com_mh stands for computed message hash */
    uint8_t com_mh[SHA256_DIGEST_LENGTH];
    SHA256(&dec[PRIVHEADER_LEN], msg_len, com_mh);

    /* check that our computed hash matches the one in the header */
    if (dec[7] != com_mh[0] || dec[8] != com_mh[1]) return 0;
    *message_length = msg_len;

    return 1;
}

/* return: 1 on success, 0 otherwise
 * in:  group, bitcoin curve
 *      enc, encryted string
 *      bn_privkey, our privkey as a bignum
 *      is_compressed??,  RETHINK
 *      ctx, bignum context
 */
static int read_public_header(const EC_GROUP *group, const uint8_t *enc,
        const uint8_t* our_pubkey, BN_CTX *ctx)
{
    if (enc[0] != 0x6a)     return 0;
    if (enc[1] != 0x6a)     return 0;
    if (enc[2] != VERSION)  return 0;
    if (enc[3] != 0x00)     return 0;
    if (enc[4] != 0x02)     return 0;

    /* computed public key hash = com_pkh */
    uint8_t com_pkh[SHA256_DIGEST_LENGTH];
    SHA256(our_pubkey, (our_pubkey[0]==0x04 ? UNCOMPR_PUBKEY_LEN : COMPR_PUBKEY_LEN), com_pkh);

    if (enc[5] != com_pkh[0] || enc[6] != com_pkh[1])
        return 0;

    return 1;
}

/* return: 1 on success, 0 otherwise
 * out: y, the y value corresponding to x+offset, if it is found
 *      offset, the offset needed to the x value to give a y
 * in:  group, bitcoin curve
 *      x,   our x
 *      odd, whether we want the y value to be odd or even,
 *      ctx, bignum context
 */
static int y_from_x(const EC_GROUP *group, BIGNUM *y, size_t *offset, const BIGNUM *x, const bool odd, BN_CTX *ctx)
{
    EC_POINT *M = EC_POINT_new(group);

    /* try to find y the easy way */
    if (EC_POINT_set_compressed_coordinates_GFp(group, M, x, odd, ctx) == 1)
    {
        EC_POINT_get_affine_coordinates_GFp(group, M, NULL, y, ctx);
        *offset = 0;
        EC_POINT_free(M);
        return 1;
    }

    int ret = 0;
    BN_CTX_start(ctx);

    BIGNUM *p = BN_CTX_get(ctx);
    BIGNUM *a = BN_CTX_get(ctx);
    BIGNUM *b = BN_CTX_get(ctx);

    EC_GROUP_get_curve_GFp(group, p, a, b, ctx);

    BIGNUM *Mx = BN_CTX_get(ctx);
    BN_copy(Mx, x);
    BIGNUM *My = BN_CTX_get(ctx);
    BIGNUM *My2 = BN_CTX_get(ctx);
    BIGNUM *aMx2 = BN_CTX_get(ctx);

    BIGNUM *half = BN_CTX_get(ctx);
    BN_copy(half, p);
    BN_add_word(half, 1);
    BN_div_word(half, 4);

    /* xoffset can be in the range 1-127 since we have 7 bits free,
       we only need 1 bit to discern odd from even points
    */
    for (int i = 1; i < 128; i++)
    {
        BN_add_word(Mx, 1);

        /* My2 = (Mx^2 * Mx mod p) */
        BN_sqr(My2, Mx, ctx);
        BN_mod_mul(My2, My2, Mx, p, ctx);

        BN_mod_sqr(aMx2, Mx, p, ctx);
        BN_mul(aMx2, aMx2, a, ctx);

        BN_mod(b, b, p, ctx);

        BN_add(My2, My2, aMx2);
        BN_add(My2, My2, b);

        BN_mod_exp(My, My2, half, p, ctx);

        /* this function will return 1 on success (point on curve), else 0 */
        if (EC_POINT_set_affine_coordinates_GFp(group, M, Mx, My, ctx) == 1)
        {
            if (odd == BN_is_bit_set(My, 0))
            {
                BN_copy(y, My);
                *offset = i;
            }
            else
            {
                BN_sub(y, p, My);
                *offset = i;
            }

            ret = 1;
            break;
        }
    }

    /* some errors here are expected since set_affine_coordinates logs an error
     * when we try to set an invalid x,y combination when trying offsets
     */
    if (ERR_peek_error())
    {
        unsigned long e = 0;
        while ((e = ERR_get_error()))
            LogPrintf("y_from_x %s: %s", ERR_func_error_string(e), ERR_reason_error_string(e));
    }

    BN_CTX_end(ctx);
    EC_POINT_free(M);

    return ret;
}

/* return:  a malloc pointer to the encrypted string,
 * out:     enc_len, size of the encrypted string in bytes
 * in:      pubkey, pointer to raw pubkey
 *          msg, pointer to the message to be encrypted
 *          msg_len, length of the message to be encrypted
 */
static uint8_t *encrypt_message(size_t *enc_len, const uint8_t *pubkey,
        const uint8_t *msg, const size_t msg_len)
{
    BN_CTX *ctx = BN_CTX_new();
    /* our secp256k1 curve and bitcoin generator
     * these must be thread local as openssl does not support shared
     * use of its data structures
     */
    EC_GROUP *group = init_curve(ctx);

    uint8_t *ret = NULL;

    EC_POINT *pk = EC_POINT_new(group);

    // get so many blocks of 32B blocks that msg will fit
    int chunk_count = (PRIVHEADER_LEN + msg_len - 1)/CHUNK_SIZE + 1;

    uint8_t *m = (uint8_t*)OPENSSL_zalloc(chunk_count * CHUNK_SIZE);

    write_private_header(m, msg, msg_len);
    memcpy(&m[PRIVHEADER_LEN], msg, msg_len);

    BIGNUM *bn_pubkey = BN_new();
    BN_bin2bn(&pubkey[1], 32, bn_pubkey);

    // pubkey is compressed
    if (pubkey[0] == 0x02 || pubkey[0] == 0x03)
    {
        EC_POINT_set_compressed_coordinates_GFp(group, pk, bn_pubkey, pubkey[0]==0x03, ctx);
    }
    else
    {
        BIGNUM *bn_pubkey_extra = BN_new();

        BN_bin2bn(&pubkey[1+32], 32, bn_pubkey_extra);
        EC_POINT_set_affine_coordinates_GFp(group, pk, bn_pubkey, bn_pubkey_extra, ctx);

        BN_free(bn_pubkey_extra);
    }

    BIGNUM *rand = BN_new();
    BIGNUM *rand_range = BN_new();
    BIGNUM *Mx = BN_new();
    BIGNUM *My = BN_new();
    EC_POINT *M = EC_POINT_new(group);
    EC_POINT *T = EC_POINT_new(group);
    EC_POINT *U = EC_POINT_new(group);
    EC_GROUP_get_order(group, rand_range, ctx);
    BN_sub_word(rand_range, 1);

    uint8_t *enc = (uint8_t*)malloc(PUBHEADER_LEN + chunk_count * 2*COMPR_PUBKEY_LEN);
    write_public_header(enc, pubkey);
    int enc_loc = PUBHEADER_LEN;
    int m_loc = 0;
    size_t xoffset = 0;

    for (int i = 0; i < chunk_count; i++)
    {
        /* since rand must be in [1,...,q-1] */
        BN_rand_range(rand, rand_range);
        BN_add_word(rand, 1);

        if (!BN_bin2bn(&m[m_loc], CHUNK_SIZE, Mx))
            goto err;

        if (!y_from_x(group, My, &xoffset, Mx, true, ctx))
            goto err;

        /* adding our xoffset that we get from y_from_x() */
        BN_add_word(Mx, xoffset);

        EC_POINT_set_affine_coordinates_GFp(group, M, Mx, My, ctx);

        /* see wiki */
        EC_POINT_mul(group, T, rand, NULL, NULL, ctx);
        EC_POINT_mul(group, U, NULL, pk, rand, ctx);
        EC_POINT_add(group, U, U, M, ctx);

        EC_POINT_point2oct(group, T, POINT_CONVERSION_COMPRESSED,
                &enc[enc_loc], COMPR_PUBKEY_LEN, ctx);
        EC_POINT_point2oct(group, U, POINT_CONVERSION_COMPRESSED,
                &enc[enc_loc+COMPR_PUBKEY_LEN], COMPR_PUBKEY_LEN, ctx);

        /* encoding our offset within the odd/even byte (02/03), the first bit represets
         * evenness and other 7 represent the offset */
        enc[enc_loc] = enc[enc_loc] - 2 + (xoffset << 1);

        enc_loc += 2*COMPR_PUBKEY_LEN;
        m_loc += CHUNK_SIZE;
    }

    /* success */
    *enc_len = enc_loc;
    ret = enc;

err:
    if (ERR_peek_error())
    {
        unsigned long e = 0;
        while ((e = ERR_get_error()))
            LogPrintf("encrypt_message %s: %s", ERR_func_error_string(e), ERR_reason_error_string(e));
    }

    #if OPENSSL_VERSION_NUMBER < 0x10100000L
    memset(m, '\0', chunk_count * CHUNK_SIZE);
    free(m);
    #else
    OPENSSL_clear_free(m, chunk_count * CHUNK_SIZE);
    #endif
    BN_free(rand);
    BN_free(rand_range);
    BN_free(Mx);
    BN_free(My);
    BN_free(bn_pubkey);
    EC_POINT_free(M);
    EC_POINT_free(T);
    EC_POINT_free(U);
    EC_POINT_free(pk);

    EC_GROUP_free(group);
    BN_CTX_free(ctx);

    return ret;
}

/* return:  malloc pointer to the decrypted message
 * out:     dec_len, length of the decrypted message
 * in:      privkey, pointer to raw private key
 *          pubkey, pointer to the raw public key matching privkey
 *          enc, pointer to the encrypted message
 *          enc_len, length of the encrypted message
 */
static uint8_t *decrypt_message(size_t *dec_len, const uint8_t *privkey, const uint8_t *pubkey,
                                const uint8_t *enc, const size_t enc_len)
{
    BN_CTX *ctx = BN_CTX_new();
    EC_GROUP *group = init_curve(ctx);

    /* check that the public header is valid and matches our priv/pubkey pair */
    if (!read_public_header(group, enc, pubkey, ctx))
        return NULL;

    int chunk_count = (enc_len - PUBHEADER_LEN) / (2*COMPR_PUBKEY_LEN);

    uint8_t *r = (uint8_t*)OPENSSL_malloc(chunk_count * CHUNK_SIZE);

    uint8_t *Tser = (uint8_t*)OPENSSL_malloc(COMPR_PUBKEY_LEN);
    uint8_t *User = (uint8_t*)OPENSSL_malloc(COMPR_PUBKEY_LEN);
    int xoffset = 0;

    EC_POINT *T = EC_POINT_new(group);
    EC_POINT *U = EC_POINT_new(group);
    EC_POINT *M = EC_POINT_new(group);
    EC_POINT *V = EC_POINT_new(group);

    BIGNUM *Mx = BN_new();

    BIGNUM *bn_privkey = BN_new();
    BN_bin2bn(privkey, PRIVKEY_LEN, bn_privkey);

    int enc_loc = PUBHEADER_LEN;
    int r_loc = 0;
    for (int i = 0; i < chunk_count; i++)
    {
        memcpy(Tser, &enc[enc_loc], COMPR_PUBKEY_LEN);
        memcpy(User, &enc[enc_loc+COMPR_PUBKEY_LEN], COMPR_PUBKEY_LEN);

        /* decode the offset and evenness from the first byte */
        xoffset = Tser[0] >> 1;
        Tser[0] = 2 + (Tser[0]&1);

        EC_POINT_oct2point(group, T, Tser, COMPR_PUBKEY_LEN, ctx);
        EC_POINT_oct2point(group, U, User, COMPR_PUBKEY_LEN, ctx);

        EC_POINT_mul(group, V, NULL, T, bn_privkey, ctx);
        EC_POINT_invert(group, V, ctx);
        EC_POINT_add(group, M, U, V, ctx);

        EC_POINT_get_affine_coordinates_GFp(group, M, Mx, NULL, ctx);

        /* substract our offset so that we get the original Mx */
        BN_sub_word(Mx, xoffset);

        BN_bn2binpad(Mx, &r[r_loc], CHUNK_SIZE);

        r_loc += CHUNK_SIZE;
        enc_loc += 2*COMPR_PUBKEY_LEN;
    }

    uint8_t *ret = NULL;

    size_t size = 0;
    if (!read_private_header(&size, r))
        goto err;

    /* success */
    ret = (uint8_t*)malloc(size);
    memcpy(ret, &r[PRIVHEADER_LEN], size);
    *dec_len = size;

err:
    if (ERR_peek_error())
    {
        unsigned long e = 0;
        while ((e = ERR_get_error()))
            LogPrintf("decrypt_message %s: %s", ERR_func_error_string(e), ERR_reason_error_string(e));
    }

    OPENSSL_free(Tser);
    OPENSSL_free(User);

    EC_POINT_clear_free(V);
    EC_POINT_clear_free(U);
    EC_POINT_clear_free(M);
    EC_POINT_clear_free(T);

    BN_free(Mx);
    BN_clear_free(bn_privkey);

    #if OPENSSL_VERSION_NUMBER < 0x10100000L
    memset(r, '\0', r_loc);
    free(r);
    #else
    OPENSSL_clear_free(r, r_loc);
    #endif

    EC_GROUP_free(group);
    BN_CTX_free(ctx);

    return ret;
}

/* wrappers for the C functions
 */
using namespace std;

namespace Jeeq {

vector<uint8_t> EncryptMessage(const CPubKey pubkey, const string msg)
{
    // check pubkey
    if (!pubkey.IsValid())
        throw runtime_error("Jeeq::EncryptMessage(): called with an invalid public key");
    //  ensure that msg is not empty
    if (msg.size() == 0) throw runtime_error("Jeeq::EncryptMessage(): no message to encrypt");

    size_t enc_len = 0;
    uint8_t *benc = encrypt_message(&enc_len, pubkey.begin(), (uint8_t*)&msg[0], msg.size());
    if (benc == NULL || enc_len == 0)
        return vector<uint8_t>{};

    vector<uint8_t> enc(benc, benc+enc_len);
    return enc;
}

string DecryptMessage(const CKey privkey, const vector<uint8_t> enc)
{
    // check privkey
    if (!privkey.IsValid())
        throw runtime_error("Jeeq::DecryptMessage(): called with an invalid private key");
    // ensure that enc is not empty
    if (enc.size() == 0)
        throw runtime_error("Jeeq::DecryptMessage(): no encoded message to decrypt");

    size_t dec_len = 0;
    CPubKey pubkey = privkey.GetPubKey();
    uint8_t *bdec = decrypt_message(&dec_len, privkey.begin(), pubkey.begin(),
                      enc.data(), enc.size());
    if (bdec == NULL || dec_len == 0)
        return string();


    string dec(bdec, bdec+dec_len);
    free(bdec);
    return dec;
}

}

// if we don't do this bignum.h ducks everything up
#include "base58.h"
#include "net.h"
#include "netbase.h"
#include "util.h"
#include "wallet.h"
#include "walletdb.h"

// auxillary function for EncryptMessage
namespace Jeeq {
CPubKey SearchForPubKey(CBitcoinAddress addr)
{
    CScript spk;
    spk.SetDestination(addr.Get());

    CBlock block;
    uint256 blockhash;
    CTransaction txout;

    // beginning at the blockchain tip going back
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev)
    {
        if (!ReadBlockFromDisk(block, pindex))
            return CPubKey();

        // skip coinbase (vtx[0])
        for (unsigned int i = 1; i < block.vtx.size(); i++)
        {
            for (unsigned int j = 0; j < block.vtx[i].vin.size(); j++)
            {
                // get tx from txid and blockhash and store it in txout
                GetTransaction(block.vtx[i].vin[j].prevout.hash, txout, blockhash);
                int n = block.vtx[i].vin[j].prevout.n;

                if (txout.vout[n].scriptPubKey == spk)
                {
                    CScript ssig = block.vtx[i].vin[j].scriptSig;

                    opcodetype opcode;
                    std::vector<unsigned char> pkdata;
                    auto pc = ssig.begin();

                    // we do it twice since the pubkey is the second item
                    ssig.GetOp(pc, opcode, pkdata);
                    ssig.GetOp(pc, opcode, pkdata);

                    return CPubKey(pkdata.begin(), pkdata.end());
                }
            }
        }
    }

    return CPubKey();
}
} // Namespace Jeeq
