#include "blockstore.h"
#include "log_iface.h"
#include "util.h"

namespace Akumuli {
namespace StorageEngine {


Block::Block(std::shared_ptr<BlockStore> bs, LogicAddr addr, std::vector<uint8_t>&& data)
    : store_(bs)
    , data_(std::move(data))
    , addr_(addr)
{
}

const uint8_t* Block::get_data() const {
    return data_.data();
}

size_t Block::get_size() const {
    return data_.size();
}


BlockStore::BlockStore(std::string metapath, std::vector<std::string> volpaths)
    : meta_(MetaVolume::open_existing(metapath.c_str()))
{
    for (uint32_t ix = 0ul; ix < volpaths.size(); ix++) {
        auto volpath = volpaths.at(ix);
        uint32_t nblocks = 0;
        aku_Status status = AKU_SUCCESS;
        std::tie(status, nblocks) = meta_->get_nblocks(ix);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, std::string("Can't open blockstore, volume " +
                                                   std::to_string(ix) + " failure: " +
                                                   // TODO: create errors.h/errors/cpp and move
                                                   // aku_error_message and error codes there
                                                   std::to_string(status)));
            AKU_PANIC("Can't open blockstore");
        }
        auto uptr = Volume::open_existing(volpath.c_str(), nblocks);
        volumes_.push_back(std::move(uptr));
        dirty_.push_back(0);
    }

    total_size_ = 0ull;
    for (const auto& vol: volumes_) {
        total_size_ += vol->get_size();
    }

    // set current volume, current volume is a first volume with free space available
    for (size_t i = 0u; i < volumes_.size(); i++) {
        uint32_t curr_gen, nblocks;
        aku_Status status;
        std::tie(status, curr_gen) = meta_->get_generation(i);
        if (status == AKU_SUCCESS) {
            std::tie(status, nblocks) = meta_->get_nblocks(i);
        }
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't find current volume, meta-volume corrupted");
            AKU_PANIC("Meta-volume corrupted");
        }
        if (volumes_[i]->get_size() > nblocks) {
            // Free space available
            current_volume_ = i;
            current_gen_ = curr_gen;
            break;
        }
    }
}

std::shared_ptr<BlockStore> BlockStore::open(std::string metapath, std::vector<std::string> volpaths) {
    auto bs = new BlockStore(metapath, volpaths);
    return std::shared_ptr<BlockStore>(bs);
}

static uint32_t extract_gen(LogicAddr addr) {
    return addr >> 32;
}

static BlockAddr extract_vol(LogicAddr addr) {
    return addr & 0xFFFFFFFF;
}

static LogicAddr make_logic(uint32_t gen, BlockAddr addr) {
    return static_cast<uint64_t>(gen) << 32 | addr;
}

bool BlockStore::exists(LogicAddr addr) const {
    auto gen = extract_gen(addr);
    auto vol = extract_vol(addr);
    auto volix = gen % volumes_.size();
    aku_Status status;
    uint32_t actual_gen;
    std::tie(status, actual_gen) = meta_->get_generation(volix);
    if (status != AKU_SUCCESS) {
        return false;
    }
    uint32_t nblocks;
    std::tie(status, nblocks) = meta_->get_nblocks(volix);
    if (status != AKU_SUCCESS) {
        return false;
    }
    return actual_gen == gen && vol < nblocks;
}

std::tuple<aku_Status, std::shared_ptr<Block>> BlockStore::read_block(LogicAddr addr) {
    aku_Status status;
    auto gen = extract_gen(addr);
    auto vol = extract_vol(addr);
    auto volix = gen % volumes_.size();
    uint32_t actual_gen;
    uint32_t nblocks;
    std::tie(status, actual_gen) = meta_->get_generation(volix);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    std::tie(status, nblocks) = meta_->get_nblocks(volix);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    if (actual_gen != gen || vol >= nblocks) {
        return std::make_tuple(AKU_EBAD_ARG, std::unique_ptr<Block>());
    }
    std::vector<uint8_t> dest(AKU_BLOCK_SIZE, 0);
    status = volumes_[volix]->read_block(vol, dest.data());
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, std::unique_ptr<Block>());
    }
    auto self = shared_from_this();
    auto block = std::make_shared<Block>(self, addr, std::move(dest));
    return std::make_tuple(status, std::move(block));
}

void BlockStore::advance_volume() {
    Logger::msg(AKU_LOG_INFO, "Advance volume called, current gen:" + std::to_string(current_gen_));
    current_volume_ = (current_volume_ + 1) % volumes_.size();
    aku_Status status;
    std::tie(status, current_gen_) = meta_->get_generation(current_volume_);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't read generation of next volume");
        AKU_PANIC("Can't read generation of the next volume");
    }
    // If volume is not empty - reset it and change generation
    uint32_t nblocks;
    std::tie(status, nblocks) = meta_->get_nblocks(current_volume_);
    if (status != AKU_SUCCESS) {
        Logger::msg(AKU_LOG_ERROR, "Can't read nblocks of next volume");
        AKU_PANIC("Can't read nblocks of the next volume");
    }
    if (nblocks != 0) {
        current_gen_ += volumes_.size();
        auto status = meta_->set_generation(current_volume_, current_gen_);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't set generation on volume");
            AKU_PANIC("Invalid BlockStore state, can't reset volume's generation");
        }
        // Rest selected volume
        status = meta_->set_nblocks(current_volume_, 0);
        if (status != AKU_SUCCESS) {
            Logger::msg(AKU_LOG_ERROR, "Can't reset nblocks on volume");
            AKU_PANIC("Invalid BlockStore state, can't reset volume's nblocks");
        }
        volumes_[current_volume_]->reset();
        dirty_[current_volume_]++;
    }
}

std::tuple<aku_Status, LogicAddr> BlockStore::append_block(uint8_t const* data) {
    BlockAddr block_addr;
    aku_Status status;
    std::tie(status, block_addr) = volumes_[current_volume_]->append_block(data);
    if (status == AKU_EOVERFLOW) {
        // Move to next generation
        advance_volume();
        std::tie(status, block_addr) = volumes_.at(current_volume_)->append_block(data);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, 0ull);
        }
        return std::make_tuple(status, make_logic(current_gen_, block_addr));
    }
    status = meta_->set_nblocks(current_volume_, block_addr + 1);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Invalid BlockStore state");
    }
    dirty_[current_volume_]++;
    return std::make_tuple(status, make_logic(current_gen_, block_addr));
}

void BlockStore::flush() {
    for (size_t ix = 0; ix < dirty_.size(); ix++) {
        if (dirty_[ix]) {
            dirty_[ix] = 0;
            volumes_[ix]->flush();
        }
    }
    meta_->flush();
}

}}  // namespace