#include <SceneMetal/FramebufferPlanRequirements.hpp>

#include <set>
#include <string_view>

namespace we::scene::metal {
namespace {

[[noreturn]] void invalidRequirement(std::string message) {
    throw Error(ErrorCode::resourceValidation, std::move(message));
}

bool depthEnabled(DepthMode mode) noexcept {
    return mode == DepthMode::enabled;
}

}  // namespace

void FramebufferPlanRequirements::requireFramebuffer(
    const FrameResourceRef& resource,
    bool requiresDepthAttachment
) {
    if (resource.kind != FrameResourceKind::framebuffer || resource.id.empty()) {
        invalidRequirement("Framebuffer requirement has an invalid resource identity");
    }
    const auto descriptor = descriptors.find(resource.id);
    if (descriptor == descriptors.end()) {
        invalidRequirement(
            "Frame plan references a framebuffer outside its descriptor set '" +
            resource.id + "'"
        );
    }
    const auto [active, inserted] = this->active.emplace(
        resource.id, descriptor->second
    );
    static_cast<void>(inserted);
    active->second.requiresDepthAttachment = active->second.requiresDepthAttachment ||
        requiresDepthAttachment;
}

FramebufferPlanRequirements analyzeFramebufferPlanRequirements(
    const FramePlan& plan
) {
    // Build and validate the complete descriptor universe first. Allocation
    // liveness must never turn malformed, currently-unused plan data into a
    // silently ignored error.
    FramebufferPlanRequirements result;
    std::map<std::string, std::string> physicalAliases;
    for (const FramebufferDescriptor& descriptor : plan.framebuffers) {
        if (descriptor.resource.kind != FrameResourceKind::framebuffer ||
            descriptor.resource.id.empty()) {
            invalidRequirement(
                "Frame plan contains an invalid framebuffer descriptor identity"
            );
        }
        const auto [inserted, didInsert] = result.descriptors.emplace(
            descriptor.resource.id,
            FramebufferAllocationRequirement{.descriptor = descriptor}
        );
        static_cast<void>(inserted);
        if (!didInsert) {
            invalidRequirement(
                "Frame plan contains a duplicate framebuffer descriptor '" +
                descriptor.resource.id + "'"
            );
        }
        physicalAliases.emplace(
            descriptor.resource.id, descriptor.resource.id
        );
    }

    const auto requireFramebuffer = [&result](
        const FrameResourceRef& resource,
        std::string_view description,
        bool requiresDepthAttachment = false
    ) {
        if (resource.kind != FrameResourceKind::framebuffer ||
            !result.descriptors.contains(resource.id)) {
            invalidRequirement(
                std::string(description) +
                " references an unknown framebuffer '" + resource.id + "'"
            );
        }
        result.requireFramebuffer(resource, requiresDepthAttachment);
    };
    const auto requireResource = [&requireFramebuffer](
        const FrameResourceRef& resource,
        std::string_view description
    ) {
        if (resource.kind == FrameResourceKind::framebuffer) {
            requireFramebuffer(resource, description);
        }
    };
    const auto requireDestination = [
        &physicalAliases,
        &requireFramebuffer,
        &result
    ](
        const FrameResourceRef& resource,
        std::string_view description,
        bool requiresDepthAttachment
    ) {
        requireFramebuffer(resource, description);
        if (!requiresDepthAttachment) return;
        const auto physical = physicalAliases.find(resource.id);
        if (physical == physicalAliases.end()) {
            invalidRequirement(
                std::string(description) +
                " has no physical framebuffer alias '" + resource.id + "'"
            );
        }
        result.requireFramebuffer({
            .kind = FrameResourceKind::framebuffer,
            .id = physical->second,
        }, true);
    };

    // Reference validation and liveness collection share one traversal. The
    // result is returned only after every operation has validated, so an error
    // later in the plan can never expose a partially analyzed allocation set.
    requireFramebuffer(plan.output, "Frame plan output");
    for (const FrameOperation& operation : plan.operations) {
        if (const auto* pass = std::get_if<FrameRenderPass>(&operation)) {
            requireDestination(
                pass->destination,
                "Frame render destination",
                depthEnabled(pass->depthTest) || depthEnabled(pass->depthWrite)
            );
            requireResource(pass->input, "Frame render input");
            if (pass->previousInput) {
                requireResource(
                    *pass->previousInput, "Frame render previous input"
                );
            }
            for (const auto& [slot, binding] : pass->textures) {
                static_cast<void>(slot);
                for (const FrameTextureCandidate& candidate :
                     binding.candidates) {
                    requireResource(
                        candidate.resource, "Frame render texture candidate"
                    );
                }
            }
        } else if (const auto* command =
                       std::get_if<FrameCopyCommand>(&operation)) {
            requireResource(command->source, "Frame copy source");
            requireFramebuffer(command->destination, "Frame copy destination");
        } else if (const auto* command =
                       std::get_if<FrameSwapCommand>(&operation)) {
            requireFramebuffer(command->source, "Frame swap source");
            requireFramebuffer(command->destination, "Frame swap destination");
            std::swap(
                physicalAliases.at(command->source.id),
                physicalAliases.at(command->destination.id)
            );
        } else if (const auto* command =
                       std::get_if<FrameClearCommand>(&operation)) {
            requireFramebuffer(command->destination, "Frame clear destination");
        } else if (const auto* command =
                       std::get_if<FrameTextCommand>(&operation)) {
            requireFramebuffer(command->destination, "Frame text destination");
        } else if (const auto* command =
                       std::get_if<FrameParticleCommand>(&operation)) {
            if (command->particleIndex >= plan.particles.size()) {
                invalidRequirement(
                    "Frame particle command references an invalid descriptor"
                );
            }
            const FrameParticleDescriptor& particle =
                plan.particles[command->particleIndex];
            requireDestination(
                command->destination,
                "Frame particle destination",
                depthEnabled(particle.depthTest) ||
                    depthEnabled(particle.depthWrite)
            );
            requireResource(particle.texture0, "Frame particle texture zero");
            for (const auto& [slot, binding] : particle.textures) {
                static_cast<void>(slot);
                for (const FrameTextureCandidate& candidate :
                     binding.candidates) {
                    requireResource(
                        candidate.resource,
                        "Frame particle texture candidate"
                    );
                }
            }
        } else {
            invalidRequirement("Frame plan contains an unknown operation variant");
        }
    }
    return result;
}

}  // namespace we::scene::metal
