/*
 * This file is part of the TREZOR project, https://trezor.io/
 *
 * Copyright (c) SatoshiLabs
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "common.h"
#include "norcow.h"
#include "storage.h"
#include "pbkdf2.h"
#include "sha2.h"
#include "hmac.h"
#include "rand.h"
#include "memzero.h"
#include "chacha20poly1305/rfc7539.h"

#define LOW_MASK            0x55555555

// The APP namespace which is reserved for storage related values.
#define APP_STORAGE         0x00

// Norcow storage key of the PIN entry log and PIN success log.
#define PIN_LOGS_KEY        ((APP_STORAGE << 8) | 0x01)

// Norcow storage key of the combined salt, EDEK, ESAK and PIN verification code entry.
#define EDEK_PVC_KEY        ((APP_STORAGE << 8) | 0x02)

// Norcow storage key of the PIN set flag.
#define PIN_NOT_SET_KEY     ((APP_STORAGE << 8) | 0x03)

// Norcow storage key of the storage version.
#define VERSION_KEY         ((APP_STORAGE << 8) | 0x04)

// Norcow storage key of the storage authentication tag.
#define STORAGE_TAG_KEY     ((APP_STORAGE << 8) | 0x05)

// The PIN value corresponding to an empty PIN.
#define PIN_EMPTY           1

// Maximum number of failed unlock attempts.
// NOTE: The PIN counter logic relies on this constant being less than or equal to 16.
#define PIN_MAX_TRIES       16

// The total number of iterations to use in PBKDF2.
#define PIN_ITER_COUNT      20000

// If the top bit of APP is set, then the value is not encrypted.
#define FLAG_PUBLIC         0x80

// The length of the guard key in words.
#define GUARD_KEY_WORDS     1

// The length of the PIN entry log or the PIN success log in words.
#define PIN_LOG_WORDS       16

// The length of a word in bytes.
#define WORD_SIZE           (sizeof(uint32_t))

// The length of the hashed hardware salt in bytes.
#define HARDWARE_SALT_SIZE  SHA256_DIGEST_LENGTH

// The length of the random salt in bytes.
#define RANDOM_SALT_SIZE    4

// The length of the data encryption key in bytes.
#define DEK_SIZE            32

// The length of the storage authentication key in bytes.
#define SAK_SIZE            16

// The combined length of the data encryption key and the storage authentication key in bytes.
#define KEYS_SIZE           (DEK_SIZE + SAK_SIZE)

// The length of the PIN verification code in bytes.
#define PVC_SIZE            8

// The length of the storage authentication tag in bytes.
#define STORAGE_TAG_SIZE    16

// The length of the Poly1305 authentication tag in bytes.
#define POLY1305_TAG_SIZE   16

// The length of the ChaCha20 IV (aka nonce) in bytes as per RFC 7539.
#define CHACHA20_IV_SIZE    12

// The length of the ChaCha20 block in bytes.
#define CHACHA20_BLOCK_SIZE 64

// Values used in the guard key integrity check.
#define GUARD_KEY_MODULUS   6311
#define GUARD_KEY_REMAINDER 15

static secbool initialized = secfalse;
static secbool unlocked = secfalse;
static PIN_UI_WAIT_CALLBACK ui_callback = NULL;
static uint8_t cached_keys[KEYS_SIZE] = {0};
static uint8_t *const cached_dek = cached_keys;
static uint8_t *const cached_sak = cached_keys + DEK_SIZE;
static uint8_t authentication_sum[SHA256_DIGEST_LENGTH] = {0};
static uint8_t hardware_salt[HARDWARE_SALT_SIZE] = {0};
static uint32_t norcow_active_version = 0;
static const uint8_t TRUE_BYTE = 0x01;
static const uint8_t FALSE_BYTE = 0x00;

static void handle_fault();
static secbool storage_upgrade();
static secbool storage_set_encrypted(const uint16_t key, const void *val, const uint16_t len);
static secbool storage_get_encrypted(const uint16_t key, void *val_dest, const uint16_t max_len, uint16_t *len);

static secbool secequal(const void* ptr1, const void* ptr2, size_t n) {
    const uint8_t* p1 = ptr1;
    const uint8_t* p2 = ptr2;
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        diff |= *p1 ^ *p2;
        ++p1;
        ++p2;
    }

    // Check loop completion in case of a fault injection attack.
    if (i != n) {
        handle_fault();
    }

    return diff ? secfalse : sectrue;
}

static secbool is_protected(uint16_t key) {
    const uint8_t app = key >> 8;
    return ((app & FLAG_PUBLIC) == 0 && app != APP_STORAGE) ? sectrue : secfalse;
}

/*
 * Initialize the storage authentication tag for freshly wiped storage.
 */
static secbool auth_init() {
    uint8_t tag[SHA256_DIGEST_LENGTH];
    memzero(authentication_sum, sizeof(authentication_sum));
    hmac_sha256(cached_sak, SAK_SIZE, authentication_sum, sizeof(authentication_sum), tag);
    return norcow_set(STORAGE_TAG_KEY, tag, STORAGE_TAG_SIZE);
}

/*
 * Update the storage authentication tag with the given key.
 */
static secbool auth_update(uint16_t key) {
    if (sectrue != is_protected(key)) {
        return sectrue;
    }

    uint8_t tag[SHA256_DIGEST_LENGTH];
    hmac_sha256(cached_sak, SAK_SIZE, (uint8_t*)&key, sizeof(key), tag);
    for (uint32_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        authentication_sum[i] ^= tag[i];
    }
    hmac_sha256(cached_sak, SAK_SIZE, authentication_sum, sizeof(authentication_sum), tag);
    return norcow_set(STORAGE_TAG_KEY, tag, STORAGE_TAG_SIZE);
}

/*
 * A secure version of norcow_set(), which updates the storage authentication tag.
 */
static secbool auth_set(uint16_t key, const void *val, uint16_t len) {
    secbool found;
    secbool ret = norcow_set_ex(key, val, len, &found);
    if (sectrue == ret && secfalse == found) {
        ret = auth_update(key);
        if (sectrue != ret) {
            norcow_delete(key);
        }
    }
    return ret;
}

/*
 * A secure version of norcow_get(), which checks the storage authentication tag.
 */
static secbool auth_get(uint16_t key, const void **val, uint16_t *len)
{
    *val = NULL;
    *len = 0;
    uint32_t sum[SHA256_DIGEST_LENGTH/sizeof(uint32_t)] = {0};

    // Prepare inner and outer digest.
    uint32_t odig[SHA256_DIGEST_LENGTH / sizeof(uint32_t)];
    uint32_t idig[SHA256_DIGEST_LENGTH / sizeof(uint32_t)];
    hmac_sha256_prepare(cached_sak, SAK_SIZE, odig, idig);

    // Prepare SHA-256 message padding.
    uint32_t g[SHA256_BLOCK_LENGTH / sizeof(uint32_t)] = {0};
    uint32_t h[SHA256_BLOCK_LENGTH / sizeof(uint32_t)] = {0};
    g[15] = (SHA256_BLOCK_LENGTH + 2) * 8;
    h[15] = (SHA256_BLOCK_LENGTH + SHA256_DIGEST_LENGTH) * 8;
    h[8] = 0x80000000;

    uint32_t offset = 0;
    uint16_t k = 0;
    uint16_t l = 0;
    uint16_t tag_len = 0;
    uint16_t entry_count = 0; // Mitigation against fault injection.
    uint16_t other_count = 0; // Mitigation against fault injection.
    const void *v = NULL;
    const void *tag_val = NULL;
    while (sectrue == norcow_get_next(&offset, &k, &v, &l)) {
        ++entry_count;
        if (k == key) {
            *val = v;
            *len = l;
        } else {
            ++other_count;
        }
        if (sectrue != is_protected(k)) {
            if (k == STORAGE_TAG_KEY) {
                tag_val = v;
                tag_len = l;
            }
            continue;
        }
        g[0] = ((k & 0xff) << 24) | ((k & 0xff00) << 8) | 0x8000; // Add SHA message padding.
        sha256_Transform(idig, g, h);
        sha256_Transform(odig, h, h);
        for (uint32_t i = 0; i < SHA256_DIGEST_LENGTH/sizeof(uint32_t); i++) {
            sum[i] ^= h[i];
        }
    }
    memcpy(h, sum, sizeof(sum));

    sha256_Transform(idig, h, h);
    sha256_Transform(odig, h, h);

    memzero(odig, sizeof(odig));
    memzero(idig, sizeof(idig));

    // Cache the authentication sum.
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH/sizeof(uint32_t); i++) {
#if BYTE_ORDER == LITTLE_ENDIAN
        REVERSE32(((uint32_t*)authentication_sum)[i], sum[i]);
#else
        ((uint32_t*)authentication_sum)[i] = sum[i];
#endif
    }

    // Check loop completion in case of a fault injection attack.
    if (secfalse != norcow_get_next(&offset, &k, &v, &l)) {
        handle_fault();
    }

    // Check storage authentication tag.
#if BYTE_ORDER == LITTLE_ENDIAN
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH/sizeof(uint32_t); i++) {
        REVERSE32(h[i], h[i]);
    }
#endif
    if (tag_val == NULL || tag_len != STORAGE_TAG_SIZE || sectrue != secequal(h, tag_val, STORAGE_TAG_SIZE)) {
        handle_fault();
    }

    if (*val == NULL) {
        // Check for fault injection.
        if (other_count != entry_count) {
            handle_fault();
        }
        return secfalse;
    }
    return sectrue;
}

/*
 *  Generates a delay of random length. Use this to protect sensitive code against fault injection.
 */
static void wait_random()
{
#ifndef TREZOR_STORAGE_TEST
    int wait = random32() & 0xff;
    volatile int i = 0;
    volatile int j = wait;
    while (i < wait) {
        if (i + j != wait) {
            handle_fault();
        }
        ++i;
        --j;
    }

    // Double-check loop completion.
    if (i != wait) {
        handle_fault();
    }
#endif
}

static void derive_kek(uint32_t pin, const uint8_t *random_salt, uint8_t kek[SHA256_DIGEST_LENGTH], uint8_t keiv[SHA256_DIGEST_LENGTH])
{
#if BYTE_ORDER == BIG_ENDIAN
    REVERSE32(pin, pin);
#endif

    uint8_t salt[HARDWARE_SALT_SIZE + RANDOM_SALT_SIZE];
    memcpy(salt, hardware_salt, HARDWARE_SALT_SIZE);
    memcpy(salt + HARDWARE_SALT_SIZE, random_salt, RANDOM_SALT_SIZE);

    PBKDF2_HMAC_SHA256_CTX ctx;
    pbkdf2_hmac_sha256_Init(&ctx, (const uint8_t*) &pin, sizeof(pin), salt, sizeof(salt), 1);
    pbkdf2_hmac_sha256_Update(&ctx, PIN_ITER_COUNT/2);
    pbkdf2_hmac_sha256_Final(&ctx, kek);
    pbkdf2_hmac_sha256_Init(&ctx, (const uint8_t*) &pin, sizeof(pin), salt, sizeof(salt), 2);
    pbkdf2_hmac_sha256_Update(&ctx, PIN_ITER_COUNT/2);
    pbkdf2_hmac_sha256_Final(&ctx, keiv);
    memzero(&ctx, sizeof(PBKDF2_HMAC_SHA256_CTX));
    memzero(&pin, sizeof(pin));
    memzero(&salt, sizeof(salt));
}

static secbool set_pin(uint32_t pin)
{
    uint8_t buffer[RANDOM_SALT_SIZE + KEYS_SIZE + POLY1305_TAG_SIZE];
    uint8_t *salt = buffer;
    uint8_t *ekeys = buffer + RANDOM_SALT_SIZE;
    uint8_t *pvc = buffer + RANDOM_SALT_SIZE + KEYS_SIZE;

    uint8_t kek[SHA256_DIGEST_LENGTH];
    uint8_t keiv[SHA256_DIGEST_LENGTH];
    chacha20poly1305_ctx ctx;
    random_buffer(salt, RANDOM_SALT_SIZE);
    derive_kek(pin, salt, kek, keiv);
    rfc7539_init(&ctx, kek, keiv);
    memzero(kek, sizeof(kek));
    memzero(keiv, sizeof(keiv));
    chacha20poly1305_encrypt(&ctx, cached_keys, ekeys, KEYS_SIZE);
    rfc7539_finish(&ctx, 0, KEYS_SIZE, pvc);
    memzero(&ctx, sizeof(ctx));
    secbool ret = norcow_set(EDEK_PVC_KEY, buffer, RANDOM_SALT_SIZE + KEYS_SIZE + PVC_SIZE);
    memzero(buffer, sizeof(buffer));

    if (ret == sectrue)
    {
        if (pin == PIN_EMPTY) {
            ret = norcow_set(PIN_NOT_SET_KEY, &TRUE_BYTE, sizeof(TRUE_BYTE));
        } else {
            ret = norcow_set(PIN_NOT_SET_KEY, &FALSE_BYTE, sizeof(FALSE_BYTE));
        }
    }

    memzero(&pin, sizeof(pin));
    return ret;
}

static secbool check_guard_key(const uint32_t guard_key)
{
    if (guard_key % GUARD_KEY_MODULUS != GUARD_KEY_REMAINDER) {
        return secfalse;
    }

    // Check that each byte of (guard_key & 0xAAAAAAAA) has exactly two bits set.
    uint32_t count = (guard_key & 0x22222222) + ((guard_key >> 2) & 0x22222222);
    count = count + (count >> 4);
    if ((count & 0x0e0e0e0e) != 0x04040404) {
        return secfalse;
    }

    // Check that the guard_key does not contain a run of 5 (or more) zeros or ones.
    uint32_t zero_runs = ~guard_key;
    zero_runs = zero_runs & (zero_runs >> 2);
    zero_runs = zero_runs & (zero_runs >> 1);
    zero_runs = zero_runs & (zero_runs >> 1);

    uint32_t one_runs = guard_key;
    one_runs = one_runs & (one_runs >> 2);
    one_runs = one_runs & (one_runs >> 1);
    one_runs = one_runs & (one_runs >> 1);

    if ((one_runs != 0) || (zero_runs != 0)) {
        return secfalse;
    }

    return sectrue;
}

static uint32_t generate_guard_key()
{
    uint32_t guard_key = 0;
    do {
        guard_key = random_uniform((UINT32_MAX/GUARD_KEY_MODULUS) + 1) * GUARD_KEY_MODULUS + GUARD_KEY_REMAINDER;
    } while (sectrue != check_guard_key(guard_key));
    return guard_key;
}

static secbool expand_guard_key(const uint32_t guard_key, uint32_t *guard_mask, uint32_t *guard)
{
    if (sectrue != check_guard_key(guard_key)) {
        handle_fault();
        return secfalse;
    }
    *guard_mask = ((guard_key & LOW_MASK) << 1) | ((~guard_key) & LOW_MASK);
    *guard = (((guard_key & LOW_MASK) << 1) & guard_key) | (((~guard_key) & LOW_MASK) & (guard_key >> 1));
    return sectrue;
}

static secbool pin_logs_init(uint32_t fails)
{
    if (fails >= PIN_MAX_TRIES) {
        return secfalse;
    }

    // The format of the PIN_LOGS_KEY entry is:
    // guard_key (1 word), pin_success_log (PIN_LOG_WORDS), pin_entry_log (PIN_LOG_WORDS)
    uint32_t logs[GUARD_KEY_WORDS + 2*PIN_LOG_WORDS];

    logs[0] = generate_guard_key();

    uint32_t guard_mask;
    uint32_t guard;
    wait_random();
    if (sectrue != expand_guard_key(logs[0], &guard_mask, &guard)) {
        return secfalse;
    }

    uint32_t unused = guard | ~guard_mask;
    for (size_t i = 0; i < 2*PIN_LOG_WORDS; ++i) {
        logs[GUARD_KEY_WORDS + i] = unused;
    }

    // Set the first word of the PIN entry log to indicate the requested number of fails.
    logs[GUARD_KEY_WORDS + PIN_LOG_WORDS] = ((((uint32_t)0xFFFFFFFF) >> (2*fails)) & ~guard_mask) | guard;

    return norcow_set(PIN_LOGS_KEY, logs, sizeof(logs));
}

/*
 * Initializes the values of VERSION_KEY, EDEK_PVC_KEY, PIN_NOT_SET_KEY and PIN_LOGS_KEY using an empty PIN.
 * This function should be called to initialize freshly wiped storage.
 */
static void init_wiped_storage()
{
    random_buffer(cached_keys, sizeof(cached_keys));
    uint32_t version = NORCOW_VERSION;
    ensure(auth_init(), "failed to initialize storage authentication tag");
    ensure(storage_set_encrypted(VERSION_KEY, &version, sizeof(version)), "failed to set storage version");
    ensure(set_pin(PIN_EMPTY), "failed to initialize PIN");
    ensure(pin_logs_init(0), "failed to initialize PIN logs");
    if (unlocked != sectrue) {
        memzero(cached_keys, sizeof(cached_keys));
    }
}

void storage_init(PIN_UI_WAIT_CALLBACK callback, const uint8_t *salt, const uint16_t salt_len)
{
    initialized = secfalse;
    unlocked = secfalse;
    norcow_init(&norcow_active_version);
    initialized = sectrue;
    ui_callback = callback;

    sha256_Raw(salt, salt_len, hardware_salt);

    if (norcow_active_version < NORCOW_VERSION) {
        if (sectrue != storage_upgrade()) {
            storage_wipe();
            ensure(secfalse, "storage_upgrade");
        }
    }

    // If there is no EDEK, then generate a random DEK and SAK and store them.
    const void *val;
    uint16_t len;
    if (secfalse == norcow_get(EDEK_PVC_KEY, &val, &len)) {
        init_wiped_storage();
    }
    memzero(cached_keys, sizeof(cached_keys));
}

static secbool pin_fails_reset()
{
    const void *logs = NULL;
    uint16_t len = 0;

    if (sectrue != norcow_get(PIN_LOGS_KEY, &logs, &len) || len != WORD_SIZE*(GUARD_KEY_WORDS + 2*PIN_LOG_WORDS)) {
        return secfalse;
    }

    uint32_t guard_mask;
    uint32_t guard;
    wait_random();
    if (sectrue != expand_guard_key(*(const uint32_t*)logs, &guard_mask, &guard)) {
        return secfalse;
    }

    uint32_t unused = guard | ~guard_mask;
    const uint32_t *success_log = ((const uint32_t*)logs) + GUARD_KEY_WORDS;
    const uint32_t *entry_log = success_log + PIN_LOG_WORDS;
    for (size_t i = 0; i < PIN_LOG_WORDS; ++i) {
        if (entry_log[i] == unused) {
            return sectrue;
        }
        if (success_log[i] != guard) {
            if (sectrue != norcow_update_word(PIN_LOGS_KEY, sizeof(uint32_t)*(i + GUARD_KEY_WORDS), entry_log[i])) {
                return secfalse;
            }
        }
    }
    return pin_logs_init(0);
}

static secbool pin_fails_increase()
{
    const void *logs = NULL;
    uint16_t len = 0;

    wait_random();
    if (sectrue != norcow_get(PIN_LOGS_KEY, &logs, &len) || len != WORD_SIZE*(GUARD_KEY_WORDS + 2*PIN_LOG_WORDS)) {
        handle_fault();
        return secfalse;
    }

    uint32_t guard_mask;
    uint32_t guard;
    wait_random();
    if (sectrue != expand_guard_key(*(const uint32_t*)logs, &guard_mask, &guard)) {
        handle_fault();
        return secfalse;
    }

    const uint32_t *entry_log = ((const uint32_t*)logs) + GUARD_KEY_WORDS + PIN_LOG_WORDS;
    for (size_t i = 0; i < PIN_LOG_WORDS; ++i) {
        wait_random();
        if ((entry_log[i] & guard_mask) != guard) {
            handle_fault();
            return secfalse;
        }
        if (entry_log[i] != guard) {
            wait_random();
            uint32_t word = entry_log[i] & ~guard_mask;
            word = ((word >> 1) | word) & LOW_MASK;
            word = (word >> 2) | (word >> 1);

            wait_random();
            if (sectrue != norcow_update_word(PIN_LOGS_KEY, sizeof(uint32_t)*(i + GUARD_KEY_WORDS + PIN_LOG_WORDS), (word & ~guard_mask) | guard)) {
                handle_fault();
                return secfalse;
            }
            return sectrue;
        }

    }
    handle_fault();
    return secfalse;
}

static uint32_t hamming_weight(uint32_t value)
{
    value = value - ((value >> 1) & 0x55555555);
    value = (value & 0x33333333) + ((value >> 2) & 0x33333333);
    value = (value + (value >> 4)) & 0x0F0F0F0F;
    value = value + (value >> 8);
    value = value + (value >> 16);
    return value & 0x3F;
}

static secbool pin_get_fails(uint32_t *ctr)
{
    *ctr = PIN_MAX_TRIES;

    const void *logs = NULL;
    uint16_t len = 0;
    wait_random();
    if (sectrue != norcow_get(PIN_LOGS_KEY, &logs, &len) || len != WORD_SIZE*(GUARD_KEY_WORDS + 2*PIN_LOG_WORDS)) {
        handle_fault();
        return secfalse;
    }

    uint32_t guard_mask;
    uint32_t guard;
    wait_random();
    if (sectrue != expand_guard_key(*(const uint32_t*)logs, &guard_mask, &guard)) {
        handle_fault();
        return secfalse;
    }
    const uint32_t unused = guard | ~guard_mask;

    const uint32_t *success_log = ((const uint32_t*)logs) + GUARD_KEY_WORDS;
    const uint32_t *entry_log = success_log + PIN_LOG_WORDS;
    volatile int current = -1;
    volatile size_t i;
    for (i = 0; i < PIN_LOG_WORDS; ++i) {
        if ((entry_log[i] & guard_mask) != guard || (success_log[i] & guard_mask) != guard || (entry_log[i] & success_log[i]) != entry_log[i]) {
            handle_fault();
            return secfalse;
        }

        if (current == -1) {
            if (entry_log[i] != guard) {
                current = i;
            }
        } else {
            if (entry_log[i] != unused) {
                handle_fault();
                return secfalse;
            }
        }
    }

    if (current < 0 || current >= PIN_LOG_WORDS || i != PIN_LOG_WORDS) {
        handle_fault();
        return secfalse;
    }

    // Strip the guard bits from the current entry word and duplicate each data bit.
    wait_random();
    uint32_t word = entry_log[current] & ~guard_mask;
    word = ((word >> 1) | word ) & LOW_MASK;
    word = word | (word << 1);
    // Verify that the entry word has form 0*1*.
    if ((word & (word + 1)) != 0) {
        handle_fault();
        return secfalse;
    }

    if (current == 0) {
        ++current;
    }

    // Count the number of set bits in the two current words of the success log.
    wait_random();
    *ctr = hamming_weight(success_log[current-1] ^ entry_log[current-1]) + hamming_weight(success_log[current] ^ entry_log[current]);
    return sectrue;
}

static secbool unlock(uint32_t pin)
{
    const void *buffer = NULL;
    uint16_t len = 0;
    if (sectrue != initialized || sectrue != norcow_get(EDEK_PVC_KEY, &buffer, &len) || len != RANDOM_SALT_SIZE + KEYS_SIZE + PVC_SIZE) {
        memzero(&pin, sizeof(pin));
        return secfalse;
    }

    const uint8_t *salt = (const uint8_t*) buffer;
    const uint8_t *ekeys = (const uint8_t*) buffer + RANDOM_SALT_SIZE;
    const uint8_t *pvc  = (const uint8_t*) buffer + RANDOM_SALT_SIZE + KEYS_SIZE;
    uint8_t kek[SHA256_DIGEST_LENGTH];
    uint8_t keiv[SHA256_DIGEST_LENGTH];
    uint8_t keys[KEYS_SIZE];
    uint8_t tag[POLY1305_TAG_SIZE];
    chacha20poly1305_ctx ctx;

    // Decrypt the data encryption key and the storage authentication key and check the PIN verification code.
    derive_kek(pin, salt, kek, keiv);
    memzero(&pin, sizeof(pin));
    rfc7539_init(&ctx, kek, keiv);
    memzero(kek, sizeof(kek));
    memzero(keiv, sizeof(keiv));
    chacha20poly1305_decrypt(&ctx, ekeys, keys, KEYS_SIZE);
    rfc7539_finish(&ctx, 0, KEYS_SIZE, tag);
    memzero(&ctx, sizeof(ctx));
    wait_random();
    if (secequal(tag, pvc, PVC_SIZE) != sectrue) {
        memzero(keys, sizeof(keys));
        memzero(tag, sizeof(tag));
        return secfalse;
    }
    memcpy(cached_keys, keys, sizeof(keys));
    memzero(keys, sizeof(keys));
    memzero(tag, sizeof(tag));

    // Call auth_get() to initialize the authentication_sum.
    auth_get(0, &buffer, &len);

    // Check that the authenticated version number matches the norcow version.
    uint32_t version;
    if (sectrue != storage_get_encrypted(VERSION_KEY, &version, sizeof(version), &len) || len != sizeof(version) || version != norcow_active_version) {
        handle_fault();
        return secfalse;
    }

    return sectrue;
}

secbool storage_unlock(uint32_t pin)
{
    // Get the pin failure counter
    uint32_t ctr;
    if (sectrue != pin_get_fails(&ctr)) {
        memzero(&pin, sizeof(pin));
        return secfalse;
    }

    // Wipe storage if too many failures
    wait_random();
    if (ctr >= PIN_MAX_TRIES) {
        storage_wipe();
        ensure(secfalse, "pin_fails_check_max");
        return secfalse;
    }

    // Sleep for 2^(ctr-1) seconds before checking the PIN.
    uint32_t wait = (1 << ctr) >> 1;
    uint32_t progress;
    for (uint32_t rem = wait; rem > 0; rem--) {
        for (int i = 0; i < 10; i++) {
            if (ui_callback) {
                if (wait > 1000000) {  // precise enough
                    progress = (wait - rem) / (wait / 1000);
                } else {
                    progress = ((wait - rem) * 10 + i) * 100 / wait;
                }
                ui_callback(rem, progress);
            }
            hal_delay(100);
        }
    }
    // Show last frame if we were waiting
    if ((wait > 0) && ui_callback) {
        ui_callback(0, 1000);
    }

    // First, we increase PIN fail counter in storage, even before checking the
    // PIN.  If the PIN is correct, we reset the counter afterwards.  If not, we
    // check if this is the last allowed attempt.
    if (sectrue != pin_fails_increase()) {
        memzero(&pin, sizeof(pin));
        return secfalse;
    }

    // Check that the PIN fail counter was incremented.
    uint32_t ctr_ck;
    if (sectrue != pin_get_fails(&ctr_ck) || ctr + 1 != ctr_ck) {
        handle_fault();
        return secfalse;
    }

    if (sectrue != unlock(pin)) {
        // Wipe storage if too many failures
        wait_random();
        if (ctr + 1 >= PIN_MAX_TRIES) {
            storage_wipe();
            ensure(secfalse, "pin_fails_check_max");
        }
        return secfalse;
    }
    memzero(&pin, sizeof(pin));
    unlocked = sectrue;

    // Finally set the counter to 0 to indicate success.
    return pin_fails_reset();
}

/*
 * Finds the encrypted data stored under key and writes its length to len.
 * If val_dest is not NULL and max_len >= len, then the data is decrypted
 * to val_dest using cached_dek as the decryption key.
 */
static secbool storage_get_encrypted(const uint16_t key, void *val_dest, const uint16_t max_len, uint16_t *len)
{
    const void *val_stored = NULL;

    if (sectrue != auth_get(key, &val_stored, len)) {
        return secfalse;
    }

    if (*len < CHACHA20_IV_SIZE + POLY1305_TAG_SIZE) {
        handle_fault();
        return secfalse;
    }
    *len -= CHACHA20_IV_SIZE + POLY1305_TAG_SIZE;

    if (val_dest == NULL) {
        return sectrue;
    }

    if (*len > max_len) {
        return secfalse;
    }

    const uint8_t *iv = (const uint8_t*) val_stored;
    const uint8_t *ciphertext = (const uint8_t*) val_stored + CHACHA20_IV_SIZE;
    const uint8_t *tag_stored = (const uint8_t*) val_stored + CHACHA20_IV_SIZE + *len;
    uint8_t tag_computed[POLY1305_TAG_SIZE];
    chacha20poly1305_ctx ctx;
    rfc7539_init(&ctx, cached_dek, iv);
    rfc7539_auth(&ctx, (const uint8_t*)&key, sizeof(key));
    chacha20poly1305_decrypt(&ctx, ciphertext, (uint8_t*) val_dest, *len);
    rfc7539_finish(&ctx, sizeof(key), *len, tag_computed);
    memzero(&ctx, sizeof(ctx));

    // Verify authentication tag.
    if (secequal(tag_computed, tag_stored, POLY1305_TAG_SIZE) != sectrue) {
        memzero(val_dest, max_len);
        memzero(tag_computed, sizeof(tag_computed));
        handle_fault();
        return secfalse;
    }

    memzero(tag_computed, sizeof(tag_computed));
    return sectrue;
}

/*
 * Finds the data stored under key and writes its length to len. If val_dest is
 * not NULL and max_len >= len, then the data is copied to val_dest.
 */
secbool storage_get(const uint16_t key, void *val_dest, const uint16_t max_len, uint16_t *len)
{
    const uint8_t app = key >> 8;
    // APP == 0 is reserved for PIN related values
    if (sectrue != initialized || app == APP_STORAGE) {
        return secfalse;
    }

    // If the top bit of APP is set, then the value is not encrypted and can be read from an unlocked device.
    secbool ret = secfalse;
    if ((app & FLAG_PUBLIC) != 0) {
        const void *val_stored = NULL;
        if (sectrue != norcow_get(key, &val_stored, len)) {
            return secfalse;
        }
        if (val_dest == NULL) {
            return sectrue;
        }
        if (*len > max_len) {
            return secfalse;
        }
        memcpy(val_dest, val_stored, *len);
        ret = sectrue;
    } else {
        if (sectrue != unlocked) {
            return secfalse;
        }
        ret = storage_get_encrypted(key, val_dest, max_len, len);
    }

    return ret;
}

/*
 * Encrypts the data at val using cached_dek as the encryption key and stores the ciphertext under key.
 */
static secbool storage_set_encrypted(const uint16_t key, const void *val, const uint16_t len)
{
    // Preallocate space on the flash storage.
    uint16_t offset = 0;
    if (sectrue != auth_set(key, NULL, CHACHA20_IV_SIZE + len + POLY1305_TAG_SIZE)) {
        return secfalse;
    }

    // Write the IV to the flash.
    uint8_t buffer[CHACHA20_BLOCK_SIZE + POLY1305_TAG_SIZE];
    random_buffer(buffer, CHACHA20_IV_SIZE);
    if (sectrue != norcow_update_bytes(key, offset, buffer, CHACHA20_IV_SIZE)) {
        return secfalse;
    }
    offset += CHACHA20_IV_SIZE;

    // Encrypt all blocks except for the last one.
    chacha20poly1305_ctx ctx;
    rfc7539_init(&ctx, cached_dek, buffer);
    rfc7539_auth(&ctx, (const uint8_t*)&key, sizeof(key));
    size_t i;
    for (i = 0; i + CHACHA20_BLOCK_SIZE < len; i += CHACHA20_BLOCK_SIZE, offset += CHACHA20_BLOCK_SIZE) {
        chacha20poly1305_encrypt(&ctx, ((const uint8_t*) val) + i, buffer, CHACHA20_BLOCK_SIZE);
        if (sectrue != norcow_update_bytes(key, offset, buffer, CHACHA20_BLOCK_SIZE)) {
            memzero(&ctx, sizeof(ctx));
            memzero(buffer, sizeof(buffer));
            return secfalse;
        }
    }

    // Encrypt final block and compute message authentication tag.
    chacha20poly1305_encrypt(&ctx, ((const uint8_t*) val) + i, buffer, len - i);
    rfc7539_finish(&ctx, sizeof(key), len, buffer + len - i);
    secbool ret = norcow_update_bytes(key, offset, buffer, len - i + POLY1305_TAG_SIZE);
    memzero(&ctx, sizeof(ctx));
    memzero(buffer, sizeof(buffer));
    return ret;
}

secbool storage_set(const uint16_t key, const void *val, const uint16_t len)
{
    const uint8_t app = key >> 8;

    // APP == 0 is reserved for PIN related values
    if (sectrue != initialized || sectrue != unlocked || app == APP_STORAGE) {
        return secfalse;
    }

    secbool ret = secfalse;
    if ((app & FLAG_PUBLIC) != 0) {
        ret = norcow_set(key, val, len);
    } else {
        ret = storage_set_encrypted(key, val, len);
    }
    return ret;
}

secbool storage_delete(const uint16_t key)
{
    const uint8_t app = key >> 8;

    // APP == 0 is reserved for storage related values
    if (sectrue != initialized || sectrue != unlocked || app == APP_STORAGE) {
        return secfalse;
    }

    secbool ret = norcow_delete(key);
    if (sectrue == ret) {
        ret = auth_update(key);
    }
    return ret;
}

secbool storage_has_pin(void)
{
    if (sectrue != initialized) {
        return secfalse;
    }

    const void *val = NULL;
    uint16_t len;
    if (sectrue != norcow_get(PIN_NOT_SET_KEY, &val, &len) || (len > 0 && *(uint8_t*)val != FALSE_BYTE)) {
        return secfalse;
    }
    return sectrue;
}

uint32_t storage_get_pin_rem(void)
{
    uint32_t ctr = 0;
    if (sectrue != pin_get_fails(&ctr)) {
        return 0;
    }
    return PIN_MAX_TRIES - ctr;
}

secbool storage_change_pin(uint32_t oldpin, uint32_t newpin)
{
    if (sectrue != initialized || sectrue != unlocked) {
        return secfalse;
    }
    if (sectrue != storage_unlock(oldpin)) {
        return secfalse;
    }
    secbool ret = set_pin(newpin);
    memzero(&oldpin, sizeof(oldpin));
    memzero(&newpin, sizeof(newpin));
    return ret;
}

void storage_wipe(void)
{
    norcow_wipe();
    norcow_active_version = NORCOW_VERSION;
    memzero(authentication_sum, sizeof(authentication_sum));
    memzero(cached_keys, sizeof(cached_keys));
    init_wiped_storage();
}

static void handle_fault()
{
    static secbool in_progress = secfalse;

    // If fault handling is already in progress, then we are probably facing a fault injection attack, so wipe.
    if (secfalse != in_progress) {
        storage_wipe();
        ensure(secfalse, "fault detected");
    }

    // We use the PIN fail counter as a fault counter. Increment the counter, check that it was incremented and halt.
    in_progress = sectrue;
    uint32_t ctr;
    if (sectrue != pin_get_fails(&ctr)) {
        storage_wipe();
        ensure(secfalse, "fault detected");
    }

    if (sectrue != pin_fails_increase()) {
        storage_wipe();
        ensure(secfalse, "fault detected");
    }

    uint32_t ctr_new;
    if (sectrue != pin_get_fails(&ctr_new) || ctr + 1 != ctr_new) {
        storage_wipe();
    }
    ensure(secfalse, "fault detected");
}

/*
 * Reads the PIN fail counter in version 0 format. Returns the current number of failed PIN entries.
 */
static secbool v0_pin_get_fails(uint32_t *ctr)
{
    const uint16_t V0_PIN_FAIL_KEY = 0x0001;
    // The PIN_FAIL_KEY points to an area of words, initialized to
    // 0xffffffff (meaning no PIN failures).  The first non-zero word
    // in this area is the current PIN failure counter.  If  PIN_FAIL_KEY
    // has no configuration or is empty, the PIN failure counter is 0.
    // We rely on the fact that flash allows to clear bits and we clear one
    // bit to indicate PIN failure.  On success, the word is set to 0,
    // indicating that the next word is the PIN failure counter.

    // Find the current pin failure counter
    const void *val = NULL;
    uint16_t len = 0;
    if (secfalse != norcow_get(V0_PIN_FAIL_KEY, &val, &len)) {
        for (unsigned int i = 0; i < len / sizeof(uint32_t); i++) {
            uint32_t word = ((const uint32_t*)val)[i];
            if (word != 0) {
                *ctr = hamming_weight(~word);
                return sectrue;
            }
        }
    }

    // No PIN failures
    *ctr = 0;
    return sectrue;
}

static secbool storage_upgrade()
{
    const uint16_t V0_PIN_KEY = 0x0000;
    const uint16_t V0_PIN_FAIL_KEY = 0x0001;
    uint16_t key = 0;
    uint16_t len = 0;
    const void *val = NULL;

    if (norcow_active_version == 0) {
        random_buffer(cached_keys, sizeof(cached_keys));

        // Initialize the storage authentication tag.
        auth_init();

        // Set the new storage version number.
        uint32_t version = NORCOW_VERSION;
        if (sectrue != storage_set_encrypted(VERSION_KEY, &version, sizeof(version))) {
            return secfalse;
        }

        // Set EDEK_PVC_KEY and PIN_NOT_SET_KEY.
        if (sectrue == norcow_get(V0_PIN_KEY, &val, &len)) {
            set_pin(*(const uint32_t*)val);
        } else {
            set_pin(PIN_EMPTY);
        }

        // Convert PIN failure counter.
        uint32_t fails = 0;
        v0_pin_get_fails(&fails);
        pin_logs_init(fails);

        // Copy the remaining entries (encrypting the protected ones).
        uint32_t offset = 0;
        while (sectrue == norcow_get_next(&offset, &key, &val, &len)) {
            if (key == V0_PIN_KEY || key == V0_PIN_FAIL_KEY) {
                continue;
            }

            secbool ret;
            if (((key >> 8) & FLAG_PUBLIC) != 0) {
                ret = norcow_set(key, val, len);
            } else {
                ret = storage_set_encrypted(key, val, len);
            }

            if (sectrue != ret) {
                return secfalse;
            }
        }

        unlocked = secfalse;
        memzero(cached_keys, sizeof(cached_keys));
    } else {
        return secfalse;
    }

    norcow_active_version = NORCOW_VERSION;
    return norcow_upgrade_finish();
}
