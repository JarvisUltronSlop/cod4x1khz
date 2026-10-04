/* Preserve the pinned upstream implementation, with a project-owned native
 * Vulkan linker correction. The upstream zlib license remains in the fetched
 * source and is reproduced in docs/third-party-vulkan/.
 */
#define MOJOSHADER_spirv_link_attributes KHZ_MOJOSHADER_original_link_attributes
#include <mojoshader_common.c>
#undef MOJOSHADER_spirv_link_attributes

void MOJOSHADER_spirv_link_attributes(const MOJOSHADER_parseData *vertex,
                                    const MOJOSHADER_parseData *pixel,
                                    int is_glspirv)
{
    if (is_glspirv) { KHZ_MOJOSHADER_original_link_attributes(vertex, pixel, is_glspirv); return; }
    SpirvPatchTable *v = (SpirvPatchTable *)&vertex->output[vertex->output_len - sizeof(SpirvPatchTable)];
    SpirvPatchTable *p = (SpirvPatchTable *)&pixel->output[pixel->output_len - sizeof(SpirvPatchTable)];
    uint32 location = 0;
    /* PS 1.x's implicit texture inputs are absent from attribute reflection.
     * The emitter's patch table includes them, so link the emitted interfaces
     * rather than leaving their placeholder Location at 0xDEADBEEF. */
    for (int usage = 0; usage < MOJOSHADER_USAGE_TOTAL; ++usage) for (int index = 0; index < 16; ++index) {
        if (usage == MOJOSHADER_USAGE_UNKNOWN || usage == MOJOSHADER_USAGE_DEPTH) continue;
        const uint32 po = p->attrib_offsets[usage][index];
        const uint32 vo = v->attrib_offsets[usage][index];
        /* Legacy parser reflection can include an input for which the emitter
         * produced no interface variable. Offset zero is absence, not word zero
         * of SPIR-V. The upstream routine overwrites the module's magic word. */
        if (!po) continue;
        ((uint32 *)pixel->output)[po] = location;
        if (vo > 4) ((uint32 *)vertex->output)[vo] = location;
        ++location;
    }
    for (int usage = 0; usage < MOJOSHADER_USAGE_TOTAL; ++usage) for (int index = 0; index < 16; ++index) {
        if ((usage == MOJOSHADER_USAGE_POSITION || usage == MOJOSHADER_USAGE_POINTSIZE) && index == 0) continue;
        if (!p->attrib_offsets[usage][index]) {
            const uint32 offset = v->attrib_offsets[usage][index];
            if (offset > 4) ((uint32 *)vertex->output)[offset] = location++;
        }
    }
    const uint32 texcoord0 = p->attrib_offsets[MOJOSHADER_USAGE_TEXCOORD][0];
    /* Legacy texture inputs have no generated PointCoord conversion. Its
     * absent offsets must not overwrite the SPIR-V version/header words. */
    if (texcoord0 && p->pointcoord_var_offset && p->pointcoord_load_offset) {
        if (v->attrib_offsets[MOJOSHADER_USAGE_POINTSIZE][0]) {
            ((uint32 *)pixel->output)[p->pointcoord_var_offset + 1] = p->tid_pvec2i;
            ((uint32 *)pixel->output)[p->pointcoord_load_offset + 1] = p->tid_vec2;
            ((uint32 *)pixel->output)[texcoord0 - 1] = SpvDecorationBuiltIn;
            ((uint32 *)pixel->output)[texcoord0] = SpvBuiltInPointCoord;
        } else {
            ((uint32 *)pixel->output)[p->pointcoord_var_offset + 1] = p->tid_pvec4i;
            ((uint32 *)pixel->output)[p->pointcoord_load_offset + 1] = p->tid_vec4;
            ((uint32 *)pixel->output)[texcoord0 - 1] = SpvDecorationLocation;
        }
    }
}
