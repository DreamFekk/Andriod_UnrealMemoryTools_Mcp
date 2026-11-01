#pragma once
#include "UERuntimeSupport.hpp"

namespace UERuntime {
struct ObjectArrayLayout {
    uintptr_t slot=0,objects=0;
    uint32_t count=0,capacity=0;
    int32_t countOffset=0;
    uint32_t itemSize=0,itemObject=0,indexOffset=0,chunkSize=0;
    int score=0;
    explicit operator bool() const {return score>=6;}
};

inline uintptr_t ObjectArrayFieldAddress(uintptr_t slot,int32_t offset) {
    if(offset<0) {
        const auto distance=uintptr_t(-int64_t(offset));
        return distance<=slot?slot-distance:0;
    }
    const auto distance=uintptr_t(offset);
    return distance<=std::numeric_limits<uintptr_t>::max()-slot?slot+distance:0;
}

inline bool ObjectArrayCountOverlapsPointer(int32_t offset) {
    return int64_t(offset)<int64_t(sizeof(uintptr_t)) && int64_t(offset)+int64_t(sizeof(int32_t))>0;
}

inline uintptr_t ArrayObject(const Reader& read,const ObjectArrayLayout& layout,uint32_t index) {
    if(index>=layout.count || !layout.itemSize) return 0;
    auto table=layout.objects;
    if(layout.chunkSize) table=Read<uintptr_t>(read,table+(index/layout.chunkSize)*sizeof(uintptr_t));
    if(!Pointer(table)) return 0;
    const auto within=layout.chunkSize?index%layout.chunkSize:index;
    return Read<uintptr_t>(read,table+uintptr_t(within)*layout.itemSize+layout.itemObject);
}

// A readable pointer or a '/Script/CoreUObject' string is not proof of an object
// table: the preallocated UObject arena contains both. Require one common
// InternalIndex offset to match each array slot across several live objects.
inline ObjectArrayLayout ValidateObjectArray(const Reader& read,uintptr_t slot,
    const std::function<bool(uintptr_t)>& readable,
    const std::function<int32_t(uintptr_t)>& indexReader={},
    uint32_t preferredStride=24,int32_t preferredCount=20,uint32_t preferredIndex=12) {
    ObjectArrayLayout best;
    const auto objects=Read<uintptr_t>(read,slot);
    if(!Pointer(objects) || !readable(objects)) return best;
    std::vector<int32_t> countOffsets{preferredCount,12,20,32,56,16,24,8};
    std::sort(countOffsets.begin(),countOffsets.end());countOffsets.erase(std::unique(countOffsets.begin(),countOffsets.end()),countOffsets.end());
    for(int32_t countOff : countOffsets) {
        // The high DWORD of a 64-bit Android pointer commonly looks like a
        // small object count. Index checks alone cannot distinguish that valid
        // prefix from the actual complete array.
        if(countOff < -64 || countOff>256 || countOff%4 || ObjectArrayCountOverlapsPointer(countOff)) continue;
        const auto countAddress=ObjectArrayFieldAddress(slot,countOff);
        if(!countAddress)continue;
        const auto count=Read<int32_t>(read,countAddress);
        if(count<16 || count>4*1024*1024) continue;
        const auto capacityAt=[&](int32_t offset) {
            if(ObjectArrayCountOverlapsPointer(offset))return int32_t(0);
            const auto address=ObjectArrayFieldAddress(slot,offset);
            return address?Read<int32_t>(read,address):int32_t(0);
        };
        const auto before=capacityAt(countOff-4),after=capacityAt(countOff+4);
        const auto plausibleCapacity=[&](int32_t value){return value>=count && value<=16*1024*1024;};
        // Standard flat/chunked layouts put MaxElements before NumElements.
        // Reordered layouts such as Delta Force put NumElements then capacity
        // after the Objects pointer (+8/+12 on the observed 64-bit build).
        const auto capacity=plausibleCapacity(before)?before:
            ((countOff!=12 && countOff!=20 && plausibleCapacity(after))?after:0);
        const bool hasCapacity=capacity!=0;
        if((countOff==12 || countOff==20) && !hasCapacity) continue;
        for(uint32_t chunkSize : {65536u,0u,16384u}) {
            if(chunkSize) {
                const auto maxChunks=Read<int32_t>(read,slot+24),chunks=Read<int32_t>(read,slot+28);
                const auto standardCapacity=Read<int32_t>(read,slot+16);
                const bool standardHeader=maxChunks>0 && maxChunks<=1024 && chunks>0 && chunks<=maxChunks &&
                    int64_t(standardCapacity)==int64_t(maxChunks)*chunkSize;
                // Do not reinterpret MaxElements as NumElements after a real
                // chunk header fails validation (or when its tail is empty).
                if(standardHeader && countOff!=20)continue;
                if(countOff==20 && (chunks<1 || chunks>1024 || maxChunks<chunks || maxChunks>1024 ||
                   uint64_t(chunks)*chunkSize<uint32_t(count) || uint64_t(maxChunks)*chunkSize<uint32_t(capacity))) continue;
            }
            const auto first=chunkSize?Read<uintptr_t>(read,objects):objects;
            if(!Pointer(first) || !readable(first)) continue;
            for(uint32_t stride : {preferredStride,16u,24u,32u,8u,20u,40u}) for(uint32_t member : {0u,8u}) {
                if(member+sizeof(uintptr_t)>stride) continue;
                ObjectArrayLayout candidate{slot,objects,uint32_t(count),hasCapacity?uint32_t(capacity):uint32_t(count),countOff,stride,member,0,chunkSize,0};
                std::array<int,33> hits{};
                int live=0,invalid=0;
                for(uint32_t i=0;i<24;++i) {
                    const auto object=ArrayObject(read,candidate,i);
                    if(!object) continue;
                    if(!Pointer(object) || !readable(object)) {if(++invalid>=3)break;continue;}
                    std::array<uint8_t,0x90> header{};
                    if(!read(object,header.data(),header.size())) {if(++invalid>=3)break;continue;}
                    const auto vtable=Read<uintptr_t>(read,object);
                    if(!Pointer(vtable) || !readable(vtable)) {if(++invalid>=3)break;continue;}
                    ++live;
                    for(uint32_t off=8;off<=0x88;off+=4) {
                        int32_t value=-1;
                        if(indexReader)value=indexReader(object+off);
                        else std::memcpy(&value,header.data()+off,sizeof(value));
                        if(value==int32_t(i)) ++hits[(off-8)/4];
                    }
                    if(live>=8)break;
                }
                if(live<6 || invalid>=3)continue;
                int maxHits=0;uint32_t indexOff=0;
                for(size_t i=0;i<hits.size();++i)if(hits[i]>maxHits || (hits[i]==maxHits && i*4+8==preferredIndex)) {
                    maxHits=hits[i];indexOff=uint32_t(i*4+8);
                }
                if(maxHits<6 || maxHits*4<live*3)continue;
                candidate.indexOffset=indexOff;candidate.score=maxHits;
                // Check separated positions, including a chunk boundary and the
                // last live range. Counts must not simply be allocation capacity.
                int distant=0;
                for(uint32_t i : {31u,127u,1023u,chunkSize?chunkSize:255u,uint32_t(count-1)}) {
                    if(i>=uint32_t(count))continue;
                    const auto object=ArrayObject(read,candidate,i);
                    if(!object)continue;
                    if(!Pointer(object) || !readable(object)) {candidate.score=0;break;}
                    const auto value=indexReader?indexReader(object+indexOff):Read<int32_t>(read,object+indexOff);
                    if(value!=int32_t(i)) {candidate.score=0;break;}
                    ++distant;
                }
                if(!candidate || distant<1)continue;
                // A fully indexed object immediately beyond the claimed end
                // proves this is only a truncated prefix. Do not silently
                // accept an arbitrary header integer as NumElements.
                auto extended=candidate;extended.count=uint32_t(count)+1;
                const auto next=ArrayObject(read,extended,uint32_t(count));
                if(Pointer(next) && readable(next)) {
                    const auto value=indexReader?indexReader(next+indexOff):Read<int32_t>(read,next+indexOff);
                    if(value==count)continue;
                }
                // Inspect the tail for compacted tables whose first indexes
                // happen to match. A valid count/capacity header may retain a
                // long empty tail after GC; that alone is not a contradiction.
                int tail=0;
                for(uint32_t distance=1;distance<=32 && distance<=uint32_t(count);++distance) {
                    const auto i=uint32_t(count)-distance;
                    const auto object=ArrayObject(read,candidate,i);
                    if(!object)continue;
                    if(!Pointer(object) || !readable(object)) {candidate.score=0;break;}
                    const auto value=indexReader?indexReader(object+indexOff):Read<int32_t>(read,object+indexOff);
                    if(value!=int32_t(i)) {candidate.score=0;break;}
                    ++tail;
                    if(tail>=2)break;
                }
                if(!candidate || (!tail && !hasCapacity))continue;
                candidate.score+=distant*3+(hasCapacity?1:0);
                if(candidate.score>best.score)best=candidate;
            }
        }
    }
    return best;
}
} // namespace UERuntime
