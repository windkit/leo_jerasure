#include <string.h>
#include <set>

#include "liberationcoding.h"

#include "jerasure.h"
#include "jerasure_mod.h"
#include "liberation.h"

void LiberationCoding::checkParams() {
    if (k <= 0 || m != 2 || w <= 0)
        throw std::invalid_argument("Invalid Coding Parameters (m = 2)");
    if (k > w)
        throw std::invalid_argument("Invalid Coding Parameters (k <= w)");
	if (w <= 2 || !(w%2) || !is_prime(w))
        throw std::invalid_argument("Invalid Coding Parameters (w is prime)");
}

vector<ERL_NIF_TERM> LiberationCoding::doEncode(ERL_NIF_TERM dataBin) {
    int *bitmatrix = liberation_coding_bitmatrix(k, w);
    int **smart = jerasure_smart_bitmatrix_to_schedule(k, m, w, bitmatrix);

    char* dataBlocks[k];
    char* codeBlocks[m];

    ErlNifBinary data;
    enif_inspect_binary(env, dataBin, &data);

    size_t dataSize = data.size;
    size_t blockSize = roundTo((roundTo(dataSize, k*w) / (k*w)), 16) * w;

    size_t offset = 0;
    size_t remain = dataSize;
    int filled = 0;
    while(remain >= blockSize) {
        dataBlocks[filled] = (char*)data.data + offset;
        offset += blockSize;
        remain -= blockSize;
        filled++;
    }
    ErlNifBinary tmp;
    enif_alloc_binary((k + m - filled) * blockSize + 16, &tmp);
    size_t align = (((size_t)data.data & 0x0f) - ((size_t)tmp.data & 0x0f) + 16) & 0x0f;
    char* alignedHead = (char*)tmp.data + align;
    memcpy(alignedHead, data.data + filled * blockSize, dataSize - filled * blockSize);
    offset = 0;
    for(int i = filled; i < k + m; ++i, offset += blockSize) {
        (i < k) ? dataBlocks[i] = alignedHead + offset:
            codeBlocks[i - k] = alignedHead + offset;
    }

    jerasure_schedule_encode(k, m, w, smart, dataBlocks, codeBlocks, blockSize, blockSize / w);

    vector<ERL_NIF_TERM> blockList;
    for(int i = 0; i < filled; ++i) {
        blockList.push_back(enif_make_sub_binary(env, dataBin, i * blockSize, blockSize)); 
    }
    ERL_NIF_TERM tmpBin = enif_make_binary(env, &tmp);
    offset = 0;
    for(int i = filled; i < k + m; ++i, offset += blockSize) {
        blockList.push_back(enif_make_sub_binary(env, tmpBin, offset + align, blockSize));
    }

    jerasure_free_schedule(smart);
    free(bitmatrix);

    return blockList;
}

ERL_NIF_TERM LiberationCoding::doDecode(vector<ERL_NIF_TERM> blockList, vector<int> blockIdList, size_t dataSize) {

    set<int> availSet(blockIdList.begin(), blockIdList.end());
    if (availSet.size() < (unsigned int)k) 
        throw std::invalid_argument("Not Enough Blocks");
    else if (availSet.size() < blockIdList.size()) {
        throw std::invalid_argument("Blocks should be unique");
    }

    size_t blockSize;

    ErlNifBinary blocks[k + m];
    for(size_t i = 0; i < blockIdList.size(); ++i) {
        int blockId = blockIdList[i];
        enif_inspect_binary(env, blockList[i], &blocks[blockId]);
        blockSize = blocks[blockId].size;
    }

    bool needFix = false;

    for(int i = 0; i < k; ++i) 
        if (availSet.count(i) == 0) {
            needFix = true;
        }


    if (!needFix) {
        ErlNifBinary file;
        enif_alloc_binary(dataSize, &file);
        size_t copySize, offset = 0;
        for(int i = 0; i < k; ++i) {
            offset = i * blockSize;
            copySize = min(dataSize - offset, blockSize);
            memcpy(file.data + offset, blocks[i].data, copySize);
        }
        ERL_NIF_TERM bin = enif_make_binary(env, &file);
        return bin;
    }

    char* dataBlocks[k];
    char* codeBlocks[m];
    int erasures[k + m];
    ErlNifBinary tmpBin;
    enif_alloc_binary(blockSize * (k + m), &tmpBin);
    char* tmpMemory = (char*)tmpBin.data;

    int j = 0;
    for(int i = 0; i < k + m; ++i) {
        i < k ? dataBlocks[i] = tmpMemory + i * blockSize : codeBlocks[i - k] = tmpMemory + i * blockSize;
        if (availSet.count(i) == 0) {
            erasures[j++] = i;
        } else {
            memcpy(tmpMemory + i * blockSize, blocks[i].data, blockSize);
        }
    }
    erasures[j] = -1;

    int *bitmatrix = liberation_coding_bitmatrix(k, w);
    jerasure_schedule_decode_data_lazy(k, m, w, bitmatrix, erasures, dataBlocks, codeBlocks, blockSize, blockSize / w, 0);

    ERL_NIF_TERM allBlocksBin = enif_make_binary(env, &tmpBin);
    ERL_NIF_TERM bin = enif_make_sub_binary(env, allBlocksBin, 0, dataSize);

    free(bitmatrix);
    return bin;
}

vector<ERL_NIF_TERM> LiberationCoding::doRepair(vector<ERL_NIF_TERM> blockList, vector<int> blockIdList, vector<int> repairList) {

    set<int> availSet(blockIdList.begin(), blockIdList.end());
    if (availSet.size() < (unsigned int)k) 
        throw std::invalid_argument("Not Enough Blocks");
    else if (availSet.size() < blockIdList.size()) {
        throw std::invalid_argument("Blocks should be unique");
    }

    size_t blockSize;

    ErlNifBinary blocks[k + m];
    for(size_t i = 0; i < blockIdList.size(); ++i) {
        int blockId = blockIdList[i];
        enif_inspect_binary(env, blockList[i], &blocks[blockId]);
        blockSize = blocks[blockId].size;
    }

    char* dataBlocks[k];
    char* codeBlocks[m];
    int erasures[k + m];
    ErlNifBinary tmpBin;
    enif_alloc_binary(blockSize * (k + m), &tmpBin);
    char* tmpMemory = (char*)tmpBin.data;

    int j = 0;
    for(int i = 0; i < k + m; ++i) {
        i < k ? dataBlocks[i] = tmpMemory + i * blockSize : codeBlocks[i - k] = tmpMemory + i * blockSize;
        if (availSet.count(i) == 0) {
            erasures[j++] = i;
        } else {
            memcpy(tmpMemory + i * blockSize, blocks[i].data, blockSize);
        }
    }
    erasures[j] = -1;

    repairList.push_back(-1);
    int *selected = &repairList[0];
    int *bitmatrix = liberation_coding_bitmatrix(k, w);
    jerasure_schedule_decode_selected_lazy(k, m, w, bitmatrix, erasures, selected, dataBlocks, codeBlocks, blockSize, blockSize / w, 0);

    vector<ERL_NIF_TERM> repairBlocks;
    int repairId;
    for(size_t i = 0; i < repairList.size() - 1; ++i) {
        repairId = repairList[i];
        ERL_NIF_TERM allBlocksBin = enif_make_binary(env, &tmpBin);
        ERL_NIF_TERM block = enif_make_sub_binary(env, allBlocksBin, repairId * blockSize, blockSize); 
        repairBlocks.push_back(block);
    }

    free(bitmatrix);
    return repairBlocks;
}
