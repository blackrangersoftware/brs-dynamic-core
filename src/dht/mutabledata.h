// Copyright (c) 2018 Duality Blockchain Solutions Developers
// TODO: Add License

#ifndef DYNAMIC_DHT_MUTABLE_DATA_H
#define DYNAMIC_DHT_MUTABLE_DATA_H

#include "bdap/bdap.h"
#include "dbwrapper.h"
#include "serialize.h"
#include "sync.h"
#include "uint256.h"

static CCriticalSection cs_dht_entry;

class CMutableData {
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    CharString vchInfoHash;  // key
    CharString vchPublicKey;
    CharString vchSignature;
    std::int64_t SequenceNumber;
    CharString vchSalt;
    CharString vchValue;

    CMutableData() {
        SetNull();
    }

    CMutableData(const CharString& infoHash, const CharString& publicKey, const CharString& signature, 
                    const std::int64_t& sequenceNumber, const CharString& salt, const CharString& value) :
                    vchInfoHash(infoHash), vchPublicKey(publicKey), vchSignature(signature), SequenceNumber(sequenceNumber), vchSalt(salt), vchValue(value){}

    inline void SetNull()
    {
        nVersion = CMutableData::CURRENT_VERSION;
        vchInfoHash.clear();
        vchPublicKey.clear();
        vchSignature.clear();
        SequenceNumber = 0;
        vchSalt.clear();
        vchValue.clear();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(this->nVersion);
        READWRITE(vchInfoHash);
        READWRITE(vchPublicKey);
        READWRITE(vchSignature);
        READWRITE(VARINT(SequenceNumber));
        READWRITE(vchSalt);
        READWRITE(vchValue);
    }

    inline friend bool operator==(const CMutableData &a, const CMutableData &b) {
        return (a.vchInfoHash == b.vchInfoHash && a.vchPublicKey == b.vchPublicKey && a.vchSalt == b.vchSalt);
    }

    inline friend bool operator!=(const CMutableData &a, const CMutableData &b) {
        return !(a == b);
    }

    inline CMutableData operator=(const CMutableData &b) {
        nVersion = b.nVersion;
        vchInfoHash = b.vchInfoHash;
        vchPublicKey = b.vchPublicKey;
        vchSignature = b.vchSignature;
        SequenceNumber = b.SequenceNumber;
        vchSalt = b.vchSalt;
        vchValue = b.vchValue;
        return *this;
    }
 
    inline bool IsNull() const { return (vchInfoHash.empty()); }
    void Serialize(std::vector<unsigned char>& vchData);
    bool UnserializeFromData(const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash);

    std::string InfoHash() const;
    std::string PublicKey() const;
    std::string Signature() const;
    std::string Salt() const;
    std::string Value() const;
};

class CMutableDataDB : public CDBWrapper {
public:
    CMutableDataDB(size_t nCacheSize, bool fMemory, bool fWipe, bool obfuscate) : CDBWrapper(GetDataDir() / "dht", nCacheSize, fMemory, fWipe, obfuscate) {
    }

    bool AddMutableData(const CMutableData& data);
    bool UpdateMutableData(const CMutableData& data);
    bool ReadMutableData(const std::vector<unsigned char>& vchInfoHash, CMutableData& data);
    bool EraseMutableData(const std::vector<unsigned char>& vchInfoHash);
    bool ListMutableData(std::vector<CMutableData>& vchMutableData);
};

bool AddMutableData(const std::vector<unsigned char>& vchInfoHash, const CMutableData& data);
bool UpdateMutableData(const std::vector<unsigned char>& vchInfoHash, const CMutableData& data);
bool GetMutableData(const std::vector<unsigned char>& vchInfoHash, CMutableData& data);
bool PutMutableData(const std::vector<unsigned char>& vchInfoHash, const CMutableData& data);
bool GetAllMutableData(std::vector<CMutableData>& vchMutableData);

extern CMutableDataDB* pMutableDataDB;

#endif // DYNAMIC_DHT_MUTABLE_DATA_H
