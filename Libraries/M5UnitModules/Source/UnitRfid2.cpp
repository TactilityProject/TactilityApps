#include <UnitRfid2.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static constexpr auto* TAG = "UnitRfid2";

const UnitRfid2::MifareKey UnitRfid2::KEY_DEFAULT = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};

// 15 commonly-found MIFARE Classic keys (from Bruce firmware / public key databases)
const UnitRfid2::MifareKey UnitRfid2::KNOWN_KEYS[15] = {
    {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }},  // factory default
    {{ 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 }},
    {{ 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5 }},
    {{ 0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD }},
    {{ 0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A }},
    {{ 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF }},
    {{ 0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97 }},
    {{ 0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F }},
    {{ 0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91 }},
    {{ 0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6 }},
    {{ 0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9 }},
    {{ 0xA6, 0x45, 0x98, 0xA7, 0x74, 0x78 }},
    {{ 0x26, 0x94, 0x0B, 0x21, 0xFF, 0x5D }},
    {{ 0xFC, 0x00, 0x01, 0x87, 0x78, 0xF7 }},
    {{ 0x00, 0x00, 0x0F, 0xFE, 0x24, 0x88 }},
};

// ---------------------------------------------------------------------------
// Low-level register access
// ---------------------------------------------------------------------------

void UnitRfid2::writeReg(uint8_t reg, uint8_t val) {
    unitWriteReg(dev_, addr_, reg, &val, 1);
}

uint8_t UnitRfid2::readReg(uint8_t reg) {
    uint8_t val = 0;
    unitReadReg(dev_, addr_, reg, &val, 1);
    return val;
}

void UnitRfid2::setBitMask(uint8_t reg, uint8_t mask) {
    writeReg(reg, readReg(reg) | mask);
}

void UnitRfid2::clearBitMask(uint8_t reg, uint8_t mask) {
    writeReg(reg, readReg(reg) & ~mask);
}

void UnitRfid2::writeFifo(const uint8_t* buf, uint8_t len) {
    for (uint8_t i = 0; i < len; i++)
        writeReg(REG_FIFO_DATA, buf[i]);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool UnitRfid2::softReset() {
    writeReg(REG_COMMAND, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < 10; i++) {
        if (!(readReg(REG_COMMAND) & 0x10)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

void UnitRfid2::antennaOn() {
    uint8_t val = readReg(REG_TX_CONTROL);
    if (!(val & 0x03))
        setBitMask(REG_TX_CONTROL, 0x03);
}

bool UnitRfid2::begin(Device* dev, uint8_t addr) {
    if (!dev || !device_is_ready(dev)) return false;
    if (!unitProbe(dev, addr)) {
        ESP_LOGW(TAG, "RFID2 not found at 0x%02X", addr);
        return false;
    }
    dev_  = dev;
    addr_ = addr;

    if (!softReset()) {
        ESP_LOGE(TAG, "RFID2 soft reset failed at 0x%02X", addr_);
        dev_ = nullptr;
        return false;
    }

    // Timer: auto mode, prescaler for ~25ms timeout
    writeReg(REG_TMODE,      0x80);
    writeReg(REG_TPRESCALER, 0xA9);
    writeReg(REG_TRELOAD_H,  0x03);
    writeReg(REG_TRELOAD_L,  0xE8);

    // 100% ASK modulation; CRC preset 0x6363
    writeReg(REG_TX_ASK, 0x40);
    writeReg(REG_MODE,   0x3D);

    antennaOn();
    ESP_LOGI(TAG, "RFID2 ready at 0x%02X", addr_);
    return true;
}

// ---------------------------------------------------------------------------
// Hardware CRC
// ---------------------------------------------------------------------------

bool UnitRfid2::calcCRC(const uint8_t* data, uint8_t len, uint8_t result[2]) {
    writeReg(REG_COMMAND, CMD_IDLE);
    writeReg(REG_DIV_IRQ, 0x04);          // clear CRCIRq
    setBitMask(REG_FIFO_LEVEL, 0x80);     // flush FIFO
    writeFifo(data, len);
    writeReg(REG_COMMAND, CMD_CALC_CRC);

    for (int i = 0; i < 500; i++) {
        if (readReg(REG_DIV_IRQ) & 0x04) {
            writeReg(REG_COMMAND, CMD_IDLE);
            result[0] = readReg(REG_CRC_RESULT_L);
            result[1] = readReg(REG_CRC_RESULT_H);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Transceive
// ---------------------------------------------------------------------------

uint8_t UnitRfid2::transceive(const uint8_t* txBuf, uint8_t txLen,
                               uint8_t* rxBuf, uint8_t rxMaxLen,
                               uint8_t* rxValidBits) {
    writeReg(REG_COM_IRQ,    0x7F);
    setBitMask(REG_FIFO_LEVEL, 0x80);
    writeReg(REG_COMMAND,    CMD_IDLE);

    writeFifo(txBuf, txLen);

    writeReg(REG_COMMAND, CMD_TRANSCEIVE);
    setBitMask(REG_BIT_FRAMING, 0x80);

    uint8_t irq = 0;
    for (int i = 0; i < 200; i++) {
        irq = readReg(REG_COM_IRQ);
        if (irq & 0x31) break;  // RxIRq | IdleIRq | TimerIRq
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    clearBitMask(REG_BIT_FRAMING, 0x80);

    if (!(irq & 0x01) && (irq & 0x30) == 0) return 0;
    if (readReg(REG_ERROR) & 0x1B) return 0;

    uint8_t n = readReg(REG_FIFO_LEVEL) & 0x7F;
    if (n > rxMaxLen) n = rxMaxLen;
    if (!rxBuf || rxMaxLen == 0) return 0;

    for (uint8_t i = 0; i < n; i++)
        rxBuf[i] = readReg(REG_FIFO_DATA);

    if (rxValidBits)
        *rxValidBits = readReg(REG_CONTROL) & 0x07;

    return n;
}

bool UnitRfid2::transceiveCRC(const uint8_t* txBuf, uint8_t txLen,
                               uint8_t* rxBuf, uint8_t rxMaxLen, uint8_t* rxLen) {
    uint8_t crc[2];
    if (!calcCRC(txBuf, txLen, crc)) return false;

    uint8_t buf[34];
    if ((size_t)txLen + 2 > sizeof(buf)) return false;
    memcpy(buf, txBuf, txLen);
    buf[txLen]     = crc[0];
    buf[txLen + 1] = crc[1];

    uint8_t n = transceive(buf, txLen + 2, rxBuf, rxMaxLen);
    if (rxLen) *rxLen = n;

    // Strip and verify CRC on response (last 2 bytes)
    if (n >= 2) {
        uint8_t rxCrc[2];
        if (!calcCRC(rxBuf, n - 2, rxCrc)) return false;
        if (rxCrc[0] != rxBuf[n - 2] || rxCrc[1] != rxBuf[n - 1]) return false;
        if (rxLen) *rxLen = n - 2;
    }
    return n > 0;
}

// ---------------------------------------------------------------------------
// ISO 14443A - REQA / WUPA and full anticollision/SELECT
// ---------------------------------------------------------------------------

// Send a 7-bit short frame command (REQA=0x26 or WUPA=0x52).
// REQA only wakes IDLE cards; WUPA wakes both IDLE and HALT cards.
bool UnitRfid2::requestA(uint8_t atqa[2], uint8_t cmd) {
    writeReg(REG_BIT_FRAMING, 0x07);
    uint8_t rx[2] = {};
    uint8_t rxLen = transceive(&cmd, 1, rx, 2);
    writeReg(REG_BIT_FRAMING, 0x00);
    if (rxLen != 2) return false;
    if (atqa) memcpy(atqa, rx, 2);
    return true;
}

// Full ISO 14443-3 anticollision + SELECT; handles 4-byte and 7-byte UIDs.
bool UnitRfid2::select(Uid* uid) {
    if (!uid) return false;

    static constexpr uint8_t selCmd[3] = { PICC_SEL_CL1, PICC_SEL_CL2, PICC_SEL_CL3 };

    uint8_t uidBytes[10] = {};
    uint8_t uidSize = 0;

    for (int cascade = 0; cascade < 3; cascade++) {
        writeReg(REG_COLL, 0x80);              // ValuesAfterColl: don't clear bits on collision
        writeReg(REG_BIT_FRAMING, 0x00);

        uint8_t anticollCmd[2] = { selCmd[cascade], PICC_ANTICOLL };
        uint8_t rx[5] = {};
        uint8_t rxLen = transceive(anticollCmd, 2, rx, 5);
        writeReg(REG_COLL, 0x00);

        if (rxLen < 5) return false;

        // Verify BCC
        uint8_t bcc = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
        if (bcc != rx[4]) return false;

        bool hasCT = (rx[0] == PICC_CT);

        // SELECT: NVB=0x70 means all 40 bits follow
        uint8_t selBuf[9];
        selBuf[0] = selCmd[cascade];
        selBuf[1] = 0x70;
        memcpy(selBuf + 2, rx, 5);  // 4 UID bytes + BCC

        uint8_t crc[2];
        if (!calcCRC(selBuf, 7, crc)) return false;
        selBuf[7] = crc[0];
        selBuf[8] = crc[1];

        uint8_t selRx[3] = {};
        uint8_t selRxLen = transceive(selBuf, 9, selRx, 3);
        // Expect SAK (1 byte) + 2 CRC bytes
        if (selRxLen < 3) return false;
        uint8_t sakCrc[2];
        if (!calcCRC(selRx, 1, sakCrc)) return false;
        if (sakCrc[0] != selRx[1] || sakCrc[1] != selRx[2]) return false;

        uint8_t sak = selRx[0];

        if (hasCT) {
            // Cascade tag byte: skip CT, copy next 3 bytes as partial UID
            memcpy(uidBytes + uidSize, rx + 1, 3);
            uidSize += 3;
        } else {
            memcpy(uidBytes + uidSize, rx, 4);
            uidSize += 4;
        }

        if (!(sak & 0x04)) {
            // UID complete (cascade bit not set)
            uid->size = uidSize;
            memcpy(uid->bytes, uidBytes, uidSize);
            uid->sak = sak;
            return true;
        }
        // Continue to next cascade level
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public card read
// ---------------------------------------------------------------------------

bool UnitRfid2::readCard(Uid* uid) {
    if (!dev_ || !uid) return false;
    uint8_t atqa[2] = {};
    // Use WUPA (0x52) so we also wake cards left in HALT state (e.g. after haltCard()).
    if (!requestA(atqa, PICC_WUPA)) return false;
    uid->atqa[0] = atqa[0];
    uid->atqa[1] = atqa[1];
    return select(uid);
}

// ---------------------------------------------------------------------------
// Legacy compatibility
// ---------------------------------------------------------------------------

bool UnitRfid2::isCardPresent() {
    if (!dev_) return false;
    uint8_t atqa[2];
    // WUPA wakes both IDLE and HALT cards, giving a reliable presence signal.
    return requestA(atqa, PICC_WUPA);
}

uint8_t UnitRfid2::readUID(uint8_t* uid, uint8_t maxLen) {
    if (!dev_ || !uid || maxLen < 4) return 0;
    Uid u;
    if (!readCard(&u)) return 0;
    uint8_t n = u.size < maxLen ? u.size : maxLen;
    memcpy(uid, u.bytes, n);
    return n;
}

// ---------------------------------------------------------------------------
// Card type detection
// ---------------------------------------------------------------------------

UnitRfid2::CardType UnitRfid2::getCardType(const Uid& uid) {
    switch (uid.sak & 0x7F) {
        case 0x09: return CardType::MifareClassicMini;
        case 0x08: return CardType::MifareClassic1K;
        case 0x18: return CardType::MifareClassic4K;
        case 0x00: break;
        default:   return CardType::Unknown;
    }

    // SAK=0x00 → Ultralight/NTAG: probe capability container at page 3
    uint8_t cc[4] = {};
    if (!ulReadPage(3, cc)) return CardType::MifareUltralight;

    switch (cc[2]) {
        case 0x12: return CardType::NTAG213;
        case 0x3E: return CardType::NTAG215;
        case 0x6D: return CardType::NTAG216;
        default:   return CardType::MifareUltralight;
    }
}

const char* UnitRfid2::cardTypeName(CardType t) {
    switch (t) {
        case CardType::MifareClassicMini: return "MIFARE Mini";
        case CardType::MifareClassic1K:   return "MIFARE Classic 1K";
        case CardType::MifareClassic4K:   return "MIFARE Classic 4K";
        case CardType::MifareUltralight:  return "MIFARE Ultralight";
        case CardType::NTAG213:           return "NTAG213";
        case CardType::NTAG215:           return "NTAG215";
        case CardType::NTAG216:           return "NTAG216";
        default:                          return "Unknown";
    }
}

uint8_t UnitRfid2::ultralightPageCount(CardType t) {
    switch (t) {
        case CardType::MifareUltralight: return 12;   // pages 4-15
        case CardType::NTAG213:          return 41;   // pages 4-44
        case CardType::NTAG215:          return 131;  // pages 4-134
        case CardType::NTAG216:          return 227;  // pages 4-230
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// MIFARE Classic - authentication
// ---------------------------------------------------------------------------

bool UnitRfid2::mfAuthenticate(uint8_t authCmd, uint8_t block,
                                const MifareKey& key, const Uid& uid) {
    stopCrypto1();

    writeReg(REG_COM_IRQ,    0x7F);
    setBitMask(REG_FIFO_LEVEL, 0x80);
    writeReg(REG_COMMAND,    CMD_IDLE);

    // FIFO payload: [authCmd, block, key[6], uid_last4[4]]
    uint8_t buf[12];
    buf[0] = authCmd;
    buf[1] = block;
    memcpy(buf + 2, key.k, 6);
    uint8_t uidOffset = (uid.size > 4) ? uid.size - 4 : 0;
    memcpy(buf + 8, uid.bytes + uidOffset, 4);

    writeFifo(buf, 12);
    writeReg(REG_COMMAND, CMD_MF_AUTHENT);

    for (int i = 0; i < 200; i++) {
        uint8_t irq = readReg(REG_COM_IRQ);
        if (irq & 0x10) break;   // IdleIRq
        if (irq & 0x01) { ESP_LOGW(TAG, "auth timeout block=%u", block); return false; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!(readReg(REG_STATUS2) & 0x08)) {
        ESP_LOGW(TAG, "MFCrypto1On not set (block=%u)", block);
        return false;
    }
    return true;
}

void UnitRfid2::stopCrypto1() {
    clearBitMask(REG_STATUS2, 0x08);
}

// ---------------------------------------------------------------------------
// MIFARE Classic - block read/write (internal, assumes auth already done)
// ---------------------------------------------------------------------------

bool UnitRfid2::mfReadBlock16(uint8_t block, uint8_t out[16]) {
    uint8_t cmd[2] = { PICC_MF_READ, block };
    uint8_t rx[18] = {};
    uint8_t rxLen = 0;
    if (!transceiveCRC(cmd, 2, rx, 18, &rxLen)) return false;
    if (rxLen < 16) return false;
    memcpy(out, rx, 16);
    return true;
}

bool UnitRfid2::mfWriteBlock16(uint8_t block, const uint8_t data[16]) {
    // Phase 1: send WRITE + block address, expect 4-bit ACK
    uint8_t cmd[2] = { PICC_MF_WRITE, block };
    uint8_t crc[2];
    if (!calcCRC(cmd, 2, crc)) return false;

    uint8_t phase1[4] = { cmd[0], cmd[1], crc[0], crc[1] };
    uint8_t ack = 0;
    uint8_t validBits = 0;
    uint8_t n = transceive(phase1, 4, &ack, 1, &validBits);
    if (n == 0 || (ack & 0x0F) != 0x0A) return false;

    // Phase 2: send 16 bytes + CRC
    if (!calcCRC(data, 16, crc)) return false;
    uint8_t phase2[18];
    memcpy(phase2, data, 16);
    phase2[16] = crc[0];
    phase2[17] = crc[1];

    ack = 0; validBits = 0;
    n = transceive(phase2, 18, &ack, 1, &validBits);
    return (n > 0 && (ack & 0x0F) == 0x0A);
}

// ---------------------------------------------------------------------------
// Public MIFARE Classic API
// ---------------------------------------------------------------------------

bool UnitRfid2::mfReadBlock(uint8_t block, const Uid& uid,
                             const MifareKey& keyA, uint8_t out[16]) {
    if (!mfAuthenticate(PICC_MF_AUTH_KEY_A, block, keyA, uid)) return false;
    return mfReadBlock16(block, out);
}

bool UnitRfid2::mfWriteBlock(uint8_t block, const Uid& uid,
                              const MifareKey& keyA, const uint8_t data[16]) {
    if (!mfAuthenticate(PICC_MF_AUTH_KEY_A, block, keyA, uid)) return false;
    return mfWriteBlock16(block, data);
}

uint8_t UnitRfid2::mfSectorBlockCount(uint8_t sector) {
    if (sector >= 40) return 0;
    return (sector < 32) ? 4 : 16;
}

bool UnitRfid2::mfReadSector(uint8_t sector, const Uid& uid,
                              const MifareKey& keyA, uint8_t* out, uint8_t* blockCountOut) {
    uint8_t count = mfSectorBlockCount(sector);
    if (count == 0) return false;

    // Sectors 0-31: 4-block layout; sectors 32-39: 16-block layout (4K).
    uint8_t firstBlock = (sector < 32) ? (sector * 4) : (128 + (sector - 32) * 16);
    if (blockCountOut) *blockCountOut = count;
    if (!mfAuthenticate(PICC_MF_AUTH_KEY_A, firstBlock, keyA, uid)) return false;
    for (uint8_t b = 0; b < count; b++) {
        if (!mfReadBlock16(firstBlock + b, out + b * 16)) return false;
    }
    return true;
}

bool UnitRfid2::mfReadBlockKeyAB(uint8_t block, const Uid& uid,
                                   const MifareKey& key, uint8_t out[16]) {
    if (mfAuthenticate(PICC_MF_AUTH_KEY_A, block, key, uid) && mfReadBlock16(block, out))
        return true;
    stopCrypto1();
    if (mfAuthenticate(PICC_MF_AUTH_KEY_B, block, key, uid) && mfReadBlock16(block, out))
        return true;
    stopCrypto1();
    return false;
}

bool UnitRfid2::mfReadBlockAuto(uint8_t block, const Uid& uid,
                                  uint8_t out[16], MifareKey* keyUsedOut) {
    for (uint8_t i = 0; i < KNOWN_KEY_COUNT; i++) {
        if (mfAuthenticate(PICC_MF_AUTH_KEY_A, block, KNOWN_KEYS[i], uid)) {
            if (mfReadBlock16(block, out)) {
                if (keyUsedOut) *keyUsedOut = KNOWN_KEYS[i];
                return true;
            }
        }
        stopCrypto1();
        if (mfAuthenticate(PICC_MF_AUTH_KEY_B, block, KNOWN_KEYS[i], uid)) {
            if (mfReadBlock16(block, out)) {
                if (keyUsedOut) *keyUsedOut = KNOWN_KEYS[i];
                return true;
            }
        }
        stopCrypto1();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Magic card UID write (gen1a backdoor)
// ---------------------------------------------------------------------------

bool UnitRfid2::mfMagicOpen() {
    // gen1a backdoor: send 0x40 (7-bit), then 0x43
    writeReg(REG_BIT_FRAMING, 0x07);
    uint8_t cmd1 = 0x40;
    uint8_t rx[1] = {};
    transceive(&cmd1, 1, rx, 1);
    writeReg(REG_BIT_FRAMING, 0x00);

    uint8_t cmd2 = 0x43;
    uint8_t rx2[1] = {};
    uint8_t n = transceive(&cmd2, 1, rx2, 1);
    return (n > 0 && rx2[0] == 0x0A);
}

bool UnitRfid2::mfWriteUid(const uint8_t newUid[4], const Uid& uid) {
    if (!dev_) return false;

    // Must wake and re-select to get into ACTIVE state
    uint8_t atqa[2];
    if (!requestA(atqa, PICC_WUPA)) return false;
    Uid localUid = uid;
    if (!select(&localUid)) return false;

    if (!mfMagicOpen()) {
        ESP_LOGW(TAG, "Magic backdoor open failed - not a gen1a card?");
        return false;
    }

    // Build block 0: UID[0..3] + BCC + SAK + ATQA[0] + ATQA[1] + 0x00*8
    uint8_t block0[16] = {};
    memcpy(block0, newUid, 4);
    block0[4] = newUid[0] ^ newUid[1] ^ newUid[2] ^ newUid[3];  // BCC
    block0[5] = uid.sak;
    block0[6] = uid.atqa[0];
    block0[7] = uid.atqa[1];

    bool ok = mfWriteBlock16(0, block0);
    haltCard();
    return ok;
}

uint8_t UnitRfid2::mfErase(const Uid& uid, const MifareKey& keyA) {
    if (!dev_) return 0;
    static const uint8_t zeros[16] = {};
    uint8_t erased = 0;

    CardType t = getCardType(uid);
    uint8_t maxBlock = 63;
    if (t == CardType::MifareClassicMini) maxBlock = 19;
    else if (t == CardType::MifareClassic4K) maxBlock = 255;

    // Initial select to put the card into ACTIVE state for sector 0 auth.
    { Uid tmp = {}; if (!readCard(&tmp)) return 0; }

    for (uint8_t block = 1; block <= maxBlock; block++) {
        // 4K: sectors 0-31 have 4 blocks (trailer at %4==3),
        //     sectors 32-39 have 16 blocks (trailer at %16==15, block >= 128).
        bool isTrailer = (block < 128) ? (block % 4 == 3) : (block % 16 == 15);
        if (isTrailer) continue;

        // Re-select card before each new sector's first data block so AUTHENT succeeds.
        bool isFirstInSector = (block < 128) ? (block % 4 == 0) : (block % 16 == 0);
        if (isFirstInSector) {
            Uid tmp = {};
            if (!readCard(&tmp)) return erased;  // card removed
        }

        if (mfWriteBlock(block, uid, keyA, zeros)) erased++;
    }
    return erased;
}

// ---------------------------------------------------------------------------
// MIFARE Ultralight / NTAG
// ---------------------------------------------------------------------------

bool UnitRfid2::ulReadPage(uint8_t page, uint8_t out4[4]) {
    // READ returns 16 bytes (4 pages); we take the first page
    uint8_t cmd[2] = { PICC_MF_READ, page };
    uint8_t rx[18] = {};
    uint8_t rxLen = 0;
    if (!transceiveCRC(cmd, 2, rx, 18, &rxLen)) return false;
    if (rxLen < 4) return false;
    memcpy(out4, rx, 4);
    return true;
}

bool UnitRfid2::ulReadPages(uint8_t startPage, uint8_t count, uint8_t* out) {
    for (uint8_t i = 0; i < count; i++) {
        if (!ulReadPage(startPage + i, out + i * 4)) return false;
    }
    return true;
}

bool UnitRfid2::ulWritePage(uint8_t page, const uint8_t data4[4], bool force) {
    // Pages 0-1: UID (factory-locked). Page 2: lock bytes. Page 3: OTP (one-time).
    // Guard against accidental writes unless the caller explicitly opts in.
    if (page < 4 && !force) {
        ESP_LOGW(TAG, "ulWritePage: page %u is UID/lock/OTP - use force=true to override", page);
        return false;
    }
    uint8_t cmd[6] = { PICC_UL_WRITE, page, data4[0], data4[1], data4[2], data4[3] };
    uint8_t crc[2];
    if (!calcCRC(cmd, 6, crc)) return false;

    uint8_t txBuf[8];
    memcpy(txBuf, cmd, 6);
    txBuf[6] = crc[0];
    txBuf[7] = crc[1];

    uint8_t ack = 0;
    uint8_t validBits = 0;
    uint8_t n = transceive(txBuf, 8, &ack, 1, &validBits);
    return (n > 0 && (ack & 0x0F) == 0x0A);
}

// ---------------------------------------------------------------------------
// HALT
// ---------------------------------------------------------------------------

bool UnitRfid2::haltA() {
    uint8_t cmd[2] = { PICC_HLTA, 0x00 };
    uint8_t crc[2];
    if (!calcCRC(cmd, 2, crc)) return false;
    uint8_t txBuf[4] = { cmd[0], cmd[1], crc[0], crc[1] };
    uint8_t rx[1];
    transceive(txBuf, 4, rx, 1);  // no response expected
    return true;
}

uint8_t UnitRfid2::ulErase(CardType t) {
    if (!dev_) return 0;
    static const uint8_t zeros[4] = {};
    uint8_t count = ultralightPageCount(t);
    if (count == 0) return 0;
    uint8_t erased = 0;
    for (uint8_t page = 4; page < 4 + count; page++) {
        if (ulWritePage(page, zeros)) erased++;
    }
    return erased;
}

void UnitRfid2::haltCard() {
    if (!dev_) return;
    haltA();
    stopCrypto1();
}
