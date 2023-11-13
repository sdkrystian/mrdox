//
// This is a derivative work. originally part of the LLVM Project.
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Copyright (c) 2023 Vinnie Falco (vinnie.falco@gmail.com)
//
// Official repository: https://github.com/cppalliance/mrdocs
//

#ifndef MRDOCS_LIB_AST_BITCODEREADER_HPP
#define MRDOCS_LIB_AST_BITCODEREADER_HPP

//
// This file implements a reader for parsing the
// mrdocs internal representation from LLVM bitcode.
// The reader takes in a stream of bits and generates
// the set of infos that it represents.
//

#include "BitcodeIDs.hpp"
#include <mrdocs/Metadata.hpp>
#include <mrdocs/Support/Error.hpp>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitstream/BitstreamReader.h>
#include <optional>

namespace clang {
namespace mrdocs {

using Record = llvm::SmallVector<uint64_t, 1024>;

class RecordReader
{
public:
    Record Data;
    llvm::StringRef Blob;
    std::size_t Index = 0;

public:
    Error
    readRecord(
        llvm::BitstreamCursor& Cursor,
        unsigned AbbrevID,
        unsigned& RecordID);

    template<typename Integral = std::uint64_t>
    requires std::integral<Integral>
    Integral readInteger();

    template<typename Integral>
    requires std::integral<Integral>
    void readInteger(Integral& result);

    bool readBool();
    void readBool(bool& result);

    template<typename Enum>
    requires std::is_enum_v<Enum>
    Enum readEnum();

    template<typename Enum>
    requires std::is_enum_v<Enum>
    void readEnum(Enum& result);

    std::string readString();
    void readString(std::string& result);

    void readLocation(Location& result);
};

// Class to read bitstream into an InfoSet collection
class BitcodeReader
{
public:
    BitcodeReader(
        llvm::BitstreamCursor& Stream)
        : Stream(Stream)
    {
    }

    // Main entry point, calls readBlock to read each block in the given stream.
    auto
    getInfos() ->
        mrdocs::Expected<std::vector<std::unique_ptr<Info>>>;
public:
    struct AnyBlock;

    enum class Cursor
    {
        BadBlock = 1,
        Record,
        BlockEnd,
        BlockBegin
    };

    Error validateStream();
    Error readBlockInfoBlock();

    /** Return the next decoded Info from the stream.
    */
    template<class T>
    mrdocs::Expected<std::unique_ptr<Info>>
    readInfo(unsigned ID);

    /** Read a single block.

        Calls readRecord on each record found.
    */
    Error readBlock(AnyBlock& B, unsigned ID);

public:
    llvm::BitstreamCursor& Stream;
    std::optional<llvm::BitstreamBlockInfo> BlockInfo;
    std::vector<AnyBlock*> blockStack_;
};

} // mrdocs
} // clang

#endif
