/*
 * UnitRfid2.h - M5Stack RFID2 Unit (WS1850S, MFRC522-compatible, I2C addr 0x28)
 *
 * 13.56 MHz RFID reader/writer. The WS1850S chip is a drop-in replacement for
 * the MFRC522 and uses the same register map.
 *
 * Supported standards: ISO/IEC 14443 Type A (MIFARE Classic, NTAG series)
 * Read range: < 20mm
 *
 * Key MFRC522 registers:
 *   0x01 CommandReg    - issue commands
 *   0x04 ComIrqReg     - interrupt flags
 *   0x05 DivIrqReg     - CRC / other irq flags
 *   0x06 ErrorReg      - error flags
 *   0x08 Status2Reg    - MFCrypto1On bit
 *   0x09 FIFODataReg   - FIFO read/write
 *   0x0A FIFOLevelReg  - bytes in FIFO
 *   0x0D BitFramingReg - bit framing for anticollision
 *   0x0E CollReg       - collision position
 *   0x11 ModeReg       - TX/RX mode, CRC preset
 *   0x14 TxControlReg  - antenna Tx1/Tx2 enable
 *   0x15 TxASKReg      - 100% ASK modulation
 *   0x21 CRCResultRegH - CRC result high byte
 *   0x22 CRCResultRegL - CRC result low byte
 *   0x2A TModeReg      - timer mode
 *   0x2B TPrescalerReg - timer prescaler
 *   0x2C TReloadRegH   - timer reload high
 *   0x2D TReloadRegL   - timer reload low
 *
 * Usage:
 *   UnitRfid2 rfid;
 *   if (rfid.begin(dev)) {
 *       UnitRfid2::Uid uid;
 *       if (rfid.readCard(&uid)) {
 *           auto type = rfid.getCardType(uid);
 *           if (type == UnitRfid2::CardType::MifareClassic1K) {
 *               uint8_t block[16];
 *               rfid.mfReadBlock(4, uid, UnitRfid2::KEY_DEFAULT, block);
 *           }
 *           rfid.haltCard();
 *       }
 *   }
 */
#pragma once

#include <UnitCommon.h>
#include <cstdint>

class UnitRfid2 {
public:
    // ---------------------------------------------------------------------------
    // Public types
    // ---------------------------------------------------------------------------

    enum class CardType : uint8_t {
        Unknown,
        MifareClassicMini,   // SAK 0x09, 5 sectors, 20 blocks
        MifareClassic1K,     // SAK 0x08, 16 sectors, 64 blocks
        MifareClassic4K,     // SAK 0x18, 40 sectors, 256 blocks
        MifareUltralight,    // SAK 0x00, 16 pages
        NTAG213,             // SAK 0x00, CC[2]=0x12, 45 pages
        NTAG215,             // SAK 0x00, CC[2]=0x3E, 135 pages
        NTAG216,             // SAK 0x00, CC[2]=0x6D, 231 pages
    };

    struct Uid {
        uint8_t size      = 0;
        uint8_t bytes[10] = {};
        uint8_t sak       = 0;
        uint8_t atqa[2]   = {};
    };

    struct MifareKey {
        uint8_t k[6] = {};
    };

    static const MifareKey KEY_DEFAULT;         // 0xFF x6
    static const MifareKey KNOWN_KEYS[15];      // common keys for auto-auth
    static constexpr uint8_t KNOWN_KEY_COUNT = 15;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    static constexpr uint8_t DEFAULT_ADDR = 0x28;

    // Pass a I2C controller device
    bool begin(Device* dev, uint8_t addr = DEFAULT_ADDR);
    bool isPresent() const { return dev_ != nullptr; }

    // ---------------------------------------------------------------------------
    // Card detection & UID
    // ---------------------------------------------------------------------------

    // Detect card, run full anticollision/select (4/7-byte UID), fill uid.
    bool readCard(Uid* uid);

    // Legacy helpers kept for compatibility.
    bool    isCardPresent();
    uint8_t readUID(uint8_t* uid, uint8_t maxLen);

    // ---------------------------------------------------------------------------
    // Card type
    // ---------------------------------------------------------------------------

    CardType    getCardType(const Uid& uid);
    const char* cardTypeName(CardType t);

    // User-accessible page count for UL/NTAG types (first user page = 4).
    // Returns 0 for MIFARE Classic types.
    uint8_t ultralightPageCount(CardType t);

    // ---------------------------------------------------------------------------
    // MIFARE Classic
    // ---------------------------------------------------------------------------

    // Read one 16-byte block; authenticates with keyA first.
    bool mfReadBlock(uint8_t block, const Uid& uid,
                     const MifareKey& keyA, uint8_t out[16]);

    // Read one block trying Key A then Key B for a given key.
    bool mfReadBlockKeyAB(uint8_t block, const Uid& uid,
                          const MifareKey& key, uint8_t out[16]);

    // Read one block trying all 15 KNOWN_KEYS (both Key A and Key B each).
    // keyUsedOut: if non-null, receives the key that succeeded.
    // Returns false if no key works.
    bool mfReadBlockAuto(uint8_t block, const Uid& uid,
                         uint8_t out[16], MifareKey* keyUsedOut = nullptr);

    // Write one 16-byte block; authenticates with keyA first.
    bool mfWriteBlock(uint8_t block, const Uid& uid,
                      const MifareKey& keyA, const uint8_t data[16]);

    // Returns 4 for sectors 0-31, 16 for sectors 32-39, 0 if sector >= 40.
    static uint8_t mfSectorBlockCount(uint8_t sector);

    // Read all blocks of a sector.
    // Sectors 0-31: 4 blocks each (MIFARE Classic 1K/4K/Mini).
    // Sectors 32-39: 16 blocks each (MIFARE Classic 4K only).
    // out must be at least mfSectorBlockCount(sector)*16 bytes; blockCount is written on success.
    bool mfReadSector(uint8_t sector, const Uid& uid,
                      const MifareKey& keyA, uint8_t* out, uint8_t* blockCount = nullptr);

    // Write a custom UID to a magic (gen1a backdoor) MIFARE card.
    // Overwrites block 0 including manufacturer data - use with care.
    // newUid must be 4 bytes; bcc is computed automatically.
    bool mfWriteUid(const uint8_t newUid[4], const Uid& uid);

    // Zero all writable data blocks (skips block 0 and sector trailers).
    // Authenticates each sector with keyA. Returns count of blocks erased.
    uint8_t mfErase(const Uid& uid, const MifareKey& keyA = KEY_DEFAULT);

    // ---------------------------------------------------------------------------
    // MIFARE Ultralight / NTAG
    // ---------------------------------------------------------------------------

    // Read one 4-byte page (chip returns 4 pages; we take the first).
    bool ulReadPage(uint8_t page, uint8_t out4[4]);

    // Read 'count' consecutive pages starting at startPage (count*4 bytes out).
    bool ulReadPages(uint8_t startPage, uint8_t count, uint8_t* out);

    // Write one 4-byte page. Pages 0-3 are UID/lock/OTP - guarded against write.
    // Pass force=true to override the guard (e.g. for clone operations).
    bool ulWritePage(uint8_t page, const uint8_t data4[4], bool force = false);

    // Zero all user pages (pages 4..4+pageCount-1). Returns count of pages erased.
    uint8_t ulErase(CardType t);

    // ---------------------------------------------------------------------------
    // Housekeeping
    // ---------------------------------------------------------------------------

    void haltCard();

private:
    Device*  dev_  = nullptr;
    uint8_t  addr_ = DEFAULT_ADDR;

    // ---------------------------------------------------------------------------
    // Low-level register access
    // ---------------------------------------------------------------------------

    void    writeReg(uint8_t reg, uint8_t val);
    uint8_t readReg(uint8_t reg);
    void    setBitMask(uint8_t reg, uint8_t mask);
    void    clearBitMask(uint8_t reg, uint8_t mask);
    void    writeFifo(const uint8_t* buf, uint8_t len);

    // ---------------------------------------------------------------------------
    // Init helpers
    // ---------------------------------------------------------------------------

    bool softReset();
    void antennaOn();

    // ---------------------------------------------------------------------------
    // CRC & transceive
    // ---------------------------------------------------------------------------

    bool    calcCRC(const uint8_t* data, uint8_t len, uint8_t result[2]);

    // Raw transceive - returns bytes received, 0 on error/timeout.
    uint8_t transceive(const uint8_t* txBuf, uint8_t txLen,
                       uint8_t* rxBuf, uint8_t rxMaxLen,
                       uint8_t* rxValidBits = nullptr);

    // Transceive with CRC appended to tx and CRC verified on rx.
    bool    transceiveCRC(const uint8_t* txBuf, uint8_t txLen,
                          uint8_t* rxBuf, uint8_t rxMaxLen, uint8_t* rxLen);

    // ---------------------------------------------------------------------------
    // ISO 14443A anticollision
    // ---------------------------------------------------------------------------

    // cmd: PICC_REQA (wakes IDLE only) or PICC_WUPA (wakes IDLE + HALT)
    bool requestA(uint8_t atqa[2], uint8_t cmd = PICC_REQA);
    bool select(Uid* uid);

    // ---------------------------------------------------------------------------
    // MIFARE internals
    // ---------------------------------------------------------------------------

    bool mfAuthenticate(uint8_t authCmd, uint8_t block,
                        const MifareKey& key, const Uid& uid);
    bool mfReadBlock16(uint8_t block, uint8_t out[16]);
    bool mfWriteBlock16(uint8_t block, const uint8_t data[16]);
    bool mfMagicOpen();   // gen1a backdoor sequence
    void stopCrypto1();
    bool haltA();

    // ---------------------------------------------------------------------------
    // MFRC522 register addresses
    // ---------------------------------------------------------------------------

    static constexpr uint8_t REG_COMMAND      = 0x01;
    static constexpr uint8_t REG_COM_IRQ      = 0x04;
    static constexpr uint8_t REG_DIV_IRQ      = 0x05;
    static constexpr uint8_t REG_ERROR        = 0x06;
    static constexpr uint8_t REG_STATUS2      = 0x08;
    static constexpr uint8_t REG_FIFO_DATA    = 0x09;
    static constexpr uint8_t REG_FIFO_LEVEL   = 0x0A;
    static constexpr uint8_t REG_CONTROL      = 0x0C;
    static constexpr uint8_t REG_BIT_FRAMING  = 0x0D;
    static constexpr uint8_t REG_COLL         = 0x0E;
    static constexpr uint8_t REG_MODE         = 0x11;
    static constexpr uint8_t REG_TX_CONTROL   = 0x14;
    static constexpr uint8_t REG_TX_ASK       = 0x15;
    static constexpr uint8_t REG_CRC_RESULT_H = 0x21;
    static constexpr uint8_t REG_CRC_RESULT_L = 0x22;
    static constexpr uint8_t REG_TMODE        = 0x2A;
    static constexpr uint8_t REG_TPRESCALER   = 0x2B;
    static constexpr uint8_t REG_TRELOAD_H    = 0x2C;
    static constexpr uint8_t REG_TRELOAD_L    = 0x2D;

    // ---------------------------------------------------------------------------
    // MFRC522 commands
    // ---------------------------------------------------------------------------

    static constexpr uint8_t CMD_IDLE         = 0x00;
    static constexpr uint8_t CMD_CALC_CRC     = 0x03;
    static constexpr uint8_t CMD_TRANSCEIVE   = 0x0C;
    static constexpr uint8_t CMD_MF_AUTHENT   = 0x0E;
    static constexpr uint8_t CMD_SOFT_RESET   = 0x0F;

    // ---------------------------------------------------------------------------
    // ISO 14443A / MIFARE PICC command bytes
    // ---------------------------------------------------------------------------

    static constexpr uint8_t PICC_REQA          = 0x26;
    static constexpr uint8_t PICC_WUPA          = 0x52;  // wakes IDLE + HALT cards
    static constexpr uint8_t PICC_HLTA          = 0x50;
    static constexpr uint8_t PICC_CT            = 0x88;
    static constexpr uint8_t PICC_SEL_CL1       = 0x93;
    static constexpr uint8_t PICC_SEL_CL2       = 0x95;
    static constexpr uint8_t PICC_SEL_CL3       = 0x97;
    static constexpr uint8_t PICC_ANTICOLL      = 0x20;
    static constexpr uint8_t PICC_MF_AUTH_KEY_A = 0x60;
    static constexpr uint8_t PICC_MF_AUTH_KEY_B = 0x61;
    static constexpr uint8_t PICC_MF_READ       = 0x30;
    static constexpr uint8_t PICC_MF_WRITE      = 0xA0;
    static constexpr uint8_t PICC_UL_WRITE      = 0xA2;
};
