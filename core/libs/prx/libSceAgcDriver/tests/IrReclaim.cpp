#include "IntermediateRepresentation/IrBuilder.hpp"
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace ShaderRecompiler;

int main() {
    try {
        IrProgram program;
        auto& block = program.CreateBlock();
        program.SetEntryBlock(block);
        program.BlockOrder().push_back(&block);
        IrBuilder builder(program);
        builder.SetInsertionPoint(block);
        auto& value = builder.Constant(42u);
        const auto id = value.Id();
        static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&value}));
        auto& identity = program.CreateValue(IrOpcode::Identity, IrType::U32);
        identity.AddArgument(&value);
        block.AppendInstruction(&identity);
        DeadCodeEliminator{}.RemoveIdentities(program);
        auto& first = program.CreateValue(IrOpcode::IAdd32, IrType::U32);
        auto& second = program.CreateValue(IrOpcode::IAdd32, IrType::U32);
        first.AddArgument(&second);
        second.AddArgument(&first);
        std::vector<IrValue*> pinned;
        for (std::uint32_t number = 100u; number < 112u; ++number) pinned.push_back(&builder.Constant(number));
        block.ssaScalarValues[0] = pinned[0];
        block.ssaVectorValues[0] = pinned[1];
        block.ssaThreadBitScalarValues[0] = pinned[2];
        block.ssaScalarMaskTags[0] = pinned[3];
        program.Metadata().dynamicReads.push_back(pinned[4]);
        program.Metadata().blockInfo.emplace_back();
        program.Metadata().blockInfo.back().condition = pinned[5];
        program.Metadata().blockInfo.back().indirectTarget = pinned[6];
        auto& resources = program.Resources();
        resources.descriptorSources.emplace_back();
        resources.descriptorSources.back().dwords[0] = pinned[7];
        resources.srtReads.push_back({pinned[8], 0u});
        resources.controlFlow.emplace_back();
        resources.controlFlow.back().condition = pinned[9];
        resources.uniformFill.values[0] = pinned[10];
        auto external = std::make_unique<IrValue>(IrOpcode::Identity, IrType::U32, 999u);
        external->AddArgument(pinned[11]);
        resources.valueStorage.push_back(std::move(external));
        std::size_t releasedBytes = 0;
        const auto removed = program.ReclaimUnusedValues(&releasedBytes);
        if (releasedBytes == 0u) throw std::runtime_error("released storage was not reported");
        if (removed != 3u) throw std::runtime_error("detached identity and cyclic garbage were not reclaimed");
        if (value.Id() != id || value.ImmediateU32() != 42u || value.Uses().size() != 1u) throw std::runtime_error("a live instruction lost its value or retained an unused backlink");
        for (std::size_t index = 0; index < pinned.size(); ++index) {
            if (pinned[index]->ImmediateU32() != 100u + index) throw std::runtime_error("a metadata or resource-owned value was reclaimed");
        }
        if (program.ReclaimUnusedValues() != 0u) throw std::runtime_error("reclaiming an unchanged graph was not idempotent");
        if (builder.Constant(999u).Id() <= pinned.back()->Id()) throw std::runtime_error("reclamation reused a value ID");
        std::puts("IR reclamation ownership tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
