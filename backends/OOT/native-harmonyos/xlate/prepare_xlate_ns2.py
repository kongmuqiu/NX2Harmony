"""The shader translator for Switch 2 (NVN2, Ampere) games, prepared on the PC as prepare_xlate_src.py does for the
Switch 1 ports, then published in the .NET container the same way:

  - the translation service is the reference's games/totk/tools/totk_xlate (Program.cs, Service.cs, VulkanReflection.cs:
    the SPIR-V + .vk/.vv output the HarmonyOS runtime reads), as for Tears of the Kingdom and Z-A;
  - the Ryujinx shader translator under it is seedds/nxnative's dm/third_party/ryujinx (MIT): Ryujinx 1.3.2 with its own
    fixes and the Ampere frontend (Decoders/DecoderAmpere.cs, Instructions/InstEmitAmpere*.cs, Transforms/TensorPass.cs)
    that Ocarina of Time needs, newer than MacNativeGames' patch 0006 (float atomics, ATOMG.CAS, HMMA chains, the
    multi-output dead-code fix of its wind solver, ...). Taken from NXNATIVE (default ../nxnative beside this backend);
  - the reference's Ryujinx patches are applied where they are not in nxnative already (0002 is, in a later form);
  - the HarmonyOS accessor answers of build_xlate.sh (TOTK_XLATE_SB_ALIGN, TOTK_XLATE_DRAW_PARAMS) and xlate_edits.py;
  - the Switch 2 answers, as MacNativeGames' translator gives them (from seedds/nxnative d3995ca4 / a7bd05f4, MIT):
    a job "arch": "sm86" sets TranslationFlags.Ampere, the job's c1 data bounds an Ampere jump table
    (QueryConstantBuffer1Size), "nvn2_cs" gives an NVN2 compute shader's own c[0] layout (QueryAmpereComputeC0Layout),
    and combined texture handles (nvnCommandBufferBindTexture, c[2] + 0x20, 32 slots) get their slot's type.

  python native-harmonyos/xlate/prepare_xlate_ns2.py <out folder>
  docker run --rm -v <out>:/src -v <bin>:/out mcr.microsoft.com/dotnet/sdk:10.0-noble-aot \\
      sh /src/games/totk/tools/totk_xlate/publish_native.sh dotnet --out /out
"""
import os, re, shutil, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from ohos_env import UPSTREAM   # noqa: E402

NXNATIVE = Path(os.environ.get('NXNATIVE') or HERE.parents[2] / 'nxnative')   # NX2Harmony sets it; else backends/nxnative
out = Path(os.path.abspath(sys.argv[1]))   # absolute (Python 3.9's resolve() leaves a missing path relative on Windows)
if out.exists():
    shutil.rmtree(out)
xl = out / 'games/totk/tools/totk_xlate'
ryu = out / 'vendor/ryujinx'
shutil.copytree(UPSTREAM / 'games/totk/tools/totk_xlate', xl, ignore=shutil.ignore_patterns('bin', 'obj'))
shutil.copytree(NXNATIVE / 'dm/third_party/ryujinx', ryu, ignore=shutil.ignore_patterns('bin', 'obj'))
TEXT = {'.cs', '.csproj', '.props', '.targets', '.patch', '.sh', '.json', '.py'}
for p in out.rglob('*'):
    if p.is_file() and p.suffix in TEXT:
        b = p.read_bytes()
        if b'\r\n' in b:
            p.write_bytes(b.replace(b'\r\n', b'\n'))
assert (ryu / 'src/Ryujinx.Graphics.Shader/Decoders/DecoderAmpere.cs').is_file(), 'nxnative Ryujinx without the Ampere frontend'


def edit(path, pairs, optional=()):
    s = path.read_bytes().decode('utf-8')
    for old, new in pairs:
        if old in optional and s.count(old) == 0:
            continue   # a code path this translator base does not have
        assert s.count(old) == 1, f'{path.name}: expected once: {old[:80]!r}'
        s = s.replace(old, new)
    path.write_bytes(s.encode('utf-8'))   # LF kept (write_text would write CRLF on Windows)


# the HarmonyOS accessor answers, word for word as build_xlate.sh adds them
script = (HERE / 'build_xlate.sh').read_text(encoding='utf-8')
add = re.search(r'add = anchor \+ """(.*?)"""', script, re.S).group(1)
anchor = "        public int QueryTextureArrayLengthFromPool() => 32;\n"
prog = xl / 'Program.cs'
assert 'QueryHasUnalignedStorageBuffer' not in prog.read_text(encoding='utf-8')
edit(prog, [(anchor, anchor + add)])

# the Switch 2 answers (MacNativeGames games/totk/tools/totk_xlate/Program.cs and Service.cs, from seedds/nxnative, MIT)
edit(prog, [
    ("    const int TexBase = 0x168, SampBase = 0x568;",
     "    // Combined handles (nvnCommandBufferBindTexture) are at c[2] + 0x20, 32 slots of 8 bytes, with the same slot numbers as the\n"
     "    // separate table (MacNativeGames Program.cs CombinedBase: Pokopia's auto-exposure fetched its histogram through one).\n"
     "    const int CombinedBase = 0x20, CombinedSlots = 32;\n"
     "    const int TexBase = 0x168, SampBase = 0x568;"),
    ("            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;",
     "            if (cbufSlot >= 0 && (cbufSlot & 0xffff) != 2) return -1;\n"
     "            if (bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0) return (bytes - TexBase) / 8;\n"
     "            if (bytes >= CombinedBase && bytes < CombinedBase + 8 * CombinedSlots && (bytes - CombinedBase) % 8 == 0) return (bytes - CombinedBase) / 8;\n"
     "            return -1;"),
    ("        public bool QueryHostReducedPrecision() => _job[\"reduced\"] is JsonNode n && (bool)n;\n",
     "        public bool QueryHostReducedPrecision() => _job[\"reduced\"] is JsonNode n && (bool)n;\n"
     "        // Switch 2 (NVN2, Ampere code; from seedds/nxnative d3995ca4 / a7bd05f4, MIT): an Ampere jump table ends at the end of the c1 data the\n"
     "        // host knows at the latest; a compute shader's own driver constant layout in c[0] (uniform buffer entries, storage buffer entries,\n"
     "        // texture handles, its size, the push constant bytes) comes with the job as nvn2_cs, read from its control section.\n"
     "        public int QueryConstantBuffer1Size() => _c1.Length;\n"
     "        public int[] QueryAmpereComputeC0Layout() => _job[\"nvn2_cs\"] is JsonArray l ? l.Select(v => (int)v).ToArray() : null;\n"
     "        // NVN2 task and mesh shaders, translated as compute (HarmonyOS port: Maleoon has no VK_EXT_mesh_shader): the stage memory's offsets in shared\n"
     "        // memory (the workgroup's number, the input, the outputs, a mesh shader's primitive bytes), as nxnative's dmsh_meshstage.py memory() gives them\n"
     "        public int[] QueryAmpereStageMemory() => _job[\"stage_memory\"] is JsonArray m ? m.Select(v => (int)v).ToArray() : null;\n"),
])
s = prog.read_bytes().decode('utf-8')
flags = "((string)job[\"stage\"] == \"cs\" ? TranslationFlags.Compute : 0);\n"
assert s.count(flags) == 1, 'Program.cs: the job flags line moved'
s = s.replace(flags, flags + "        if ((string)job[\"arch\"] == \"sm86\") flags |= TranslationFlags.Ampere;   // NVN2 (Switch 2): a 0x80-byte SPH and 128-bit instructions\n")
if 'using System.Linq;' not in s:
    s = 'using System.Linq;\n' + s
prog.write_bytes(s.encode('utf-8'))

svc = xl / 'Service.cs'
edit(svc, [(
    "        job[\"early_fragment_tests\"] = stage == \"ps\" && Get(rq, \"early_fragment_tests\") is JsonValue ef && ef.TryGetValue<bool>(out bool early) && early;\n",
    "        job[\"early_fragment_tests\"] = stage == \"ps\" && Get(rq, \"early_fragment_tests\") is JsonValue ef && ef.TryGetValue<bool>(out bool early) && early;\n"
    "        if (Get(rq, \"arch\") is JsonValue arch) job[\"arch\"] = arch.DeepClone();   // \"sm86\": Ampere code (NVN2, Switch 2); absent: Maxwell\n"
    "        if (Get(rq, \"nvn2_cs\") is JsonArray cs2) job[\"nvn2_cs\"] = cs2.DeepClone();   // an NVN2 compute shader's c[0] layout (Program.cs)\n"
    "        if (Get(rq, \"shared\") is JsonValue sh2) job[\"shared\"] = sh2.DeepClone();   // compute shared memory bytes when the request knows them\n"
    "        if (Get(rq, \"stage_memory\") is JsonArray sm) job[\"stage_memory\"] = sm.DeepClone();   // a task or mesh shader's stage memory (Program.cs)\n")])

sys.path.insert(0, str(HERE))
import xlate_edits   # noqa: E402
xlate_edits.base_instance_only(ryu)

# TXQ for a size component the texture's type does not have (Ocarina of Time: the depth of a 2D texture, 74 vertex and
# pixel shaders): the translator extracted it from the smaller vector (invalid SPIR-V, spirv-val "Vector access is out of
# bounds"); NVIDIA's texture header answers 1 there
anchor = "                SpvInstruction resultType = dimensions == 1 ? context.TypeS32() : context.TypeVector(context.TypeS32(), dimensions);\n"
edit(ryu / 'src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Instructions.cs', [(anchor,
    "                if (texOp.Index >= dimensions)\n"
    "                {\n"
    "                    return new OperationResult(AggregateType.S32, context.Constant(context.TypeS32(), 1));   // a component the type lacks (HarmonyOS port)\n"
    "                }\n\n" + anchor)])

# Global memory without Int64 (Ocarina of Time: 2138 of 42327 shaders reach memory through a loaded address; Maleoon has
# no shaderInt64 and refuses them, "SPV01: Unsupported capability (11)"). nxnative built the physical storage buffer
# pointer with OpConvertUToPtr from a 64-bit integer (the address's two words bitcast to uint64). SPIR-V 1.5 lets OpBitcast
# turn a vector of two 32-bit integers into a 64-bit pointer (GL_EXT_buffer_reference_uvec2), so those modules are 1.5
# (the device is Vulkan 1.3) and declare no Int64. From 1.4: the entry point lists every global variable it uses, and
# storage buffers are StorageBuffer + Block (BufferBlock is gone). Every other module stays 1.3, byte for byte as before.
src = ryu / 'src'
edit(src / 'Spv.Generator/Module.cs', [
    ("        private readonly uint _version;\n",
     "        private uint _version;\n"
     "        private readonly List<Instruction[]> _entrypointInterfaces = [];   // HarmonyOS port: completed for SPIR-V 1.4+ in Generate\n\n"
     "        public uint Version => _version;\n\n"
     "        public void SetVersion(uint version) => _version = version;   // HarmonyOS port: before anything is declared\n"),
    ("            _entrypoints.Add(entryPoint);\n",
     "            _entrypoints.Add(entryPoint);\n"
     "            _entrypointInterfaces.Add(interfaces.ToArray());\n"),
    ("            // 5.\n"
     "            foreach (Instruction entrypoint in _entrypoints)\n",
     "            // HarmonyOS port: from SPIR-V 1.4 an entry point's interface lists every global variable, not only inputs and outputs\n"
     "            if (_version >= 0x10400)\n"
     "            {\n"
     "                for (int i = 0; i < _entrypoints.Count; i++)\n"
     "                {\n"
     "                    List<Instruction> more = [];\n"
     "                    foreach (Instruction global in _globals)\n"
     "                    {\n"
     "                        if (Array.IndexOf(_entrypointInterfaces[i], global) < 0)\n"
     "                        {\n"
     "                            more.Add(global);\n"
     "                        }\n"
     "                    }\n\n"
     "                    if (more.Count != 0)\n"
     "                    {\n"
     "                        _entrypoints[i].AddOperand(more.ToArray());\n"
     "                        more.InsertRange(0, _entrypointInterfaces[i]);\n"
     "                        _entrypointInterfaces[i] = more.ToArray();\n"
     "                    }\n"
     "                }\n"
     "            }\n\n"
     "            // 5.\n"
     "            foreach (Instruction entrypoint in _entrypoints)\n")])
shd = src / 'Ryujinx.Graphics.Shader/CodeGen/Spirv'
edit(shd / 'SpirvGenerator.cs', [(
    "                context.AddCapability(Capability.Int64);\n",
    "                context.SetVersion(0x10500);   // HarmonyOS port: OpBitcast of the address's two words to the pointer, no Int64\n")])
ptr_from_words = [
    ("            SpvInstruction pointer = context.ConvertUToPtr(\n"
     "                context.TypePointer(StorageClass.PhysicalStorageBuffer, vectorType),\n"
     "                context.Bitcast(context.TypeInt(64, 0), addressWords));\n",
     "            SpvInstruction pointer = context.Bitcast(context.TypePointer(StorageClass.PhysicalStorageBuffer, vectorType), addressWords);   // SPIR-V 1.5, no Int64 (HarmonyOS port)\n"),
    ("                    return context.ConvertUToPtr(\n"
     "                        context.TypePointer(StorageClass.PhysicalStorageBuffer, context.TypeU32()),\n"
     "                        context.Bitcast(context.TypeInt(64, 0), addressWords));\n",
     "                    return context.Bitcast(context.TypePointer(StorageClass.PhysicalStorageBuffer, context.TypeU32()), addressWords);   // SPIR-V 1.5, no Int64 (HarmonyOS port)\n"),
    ("                    storageClass = StorageClass.Uniform;\n",
     "                    storageClass = storageKind == StorageKind.StorageBuffer && context.Version >= 0x10400 ? StorageClass.StorageBuffer : StorageClass.Uniform;   // HarmonyOS port: 1.4+\n")]
# Swap2-Native's translator (mamcjr/Swap2-Native, MIT: nxnative's with the Android port's fixes and mesh shaders) chooses the
# buffer storage class in one place, CodeGenContext.BufferStorageClass (StorageBuffer for its SPIR-V 1.4 mesh modules): the
# condition becomes the module's version, and its Declarations and Instructions follow. It has no raw vector load.
SWAP2 = 'BufferStorageClass(bool isBuffer)' in (shd / 'CodeGenContext.cs').read_text(encoding='utf-8')
if SWAP2:
    edit(shd / 'CodeGenContext.cs', [(
        "            isBuffer && Properties.Mesh != null ? Spv.Specification.StorageClass.StorageBuffer : Spv.Specification.StorageClass.Uniform;\n",
        "            isBuffer && Version >= 0x10400 ? Spv.Specification.StorageClass.StorageBuffer : Spv.Specification.StorageClass.Uniform;   // HarmonyOS port: every 1.4+ module\n")])
    ptr_from_words = ptr_from_words[:2]
print('translator base:', 'Swap2-Native' if SWAP2 else 'nxnative')
edit(shd / 'Instructions.cs', ptr_from_words, optional=(ptr_from_words[0][0],))

# Bindless texture pools (Ocarina of Time: about 4800 shaders sample the texture pool by an ID loaded from memory; nxnative
# binds the whole pools on Metal). Ryujinx turned those into arrays of QueryTextureArrayLengthFromPool() (32 here) indexed by
# min(id, 31), which the replay filled with dummies: every material read zero and the 3D scene came out black. With
# TOTK_XLATE_POOL_SET / _TEX / _SAMP in the translator's environment (oot_seed.py, fill_shaders_ohos.py: 2, 33068, 4096,
# the game's pool sizes), the arrays of the types the replay binds as whole pools take the pools' lengths and go to that
# descriptor set at fixed bindings: 0 the samplers, 1 2D, 2 2D array, 3 3D, 4 cube, 5 cube array textures, indexed
# non-uniformly. The other types (1D, multisample) keep 32-element arrays in the stage's set. Unset: as before.
edit(prog, [
    ("        public int QuerySamplerArrayLengthFromPool() => 32;\n",
     "        // HarmonyOS port: the game's pool sizes for the bindless arrays (ResourceManager.BindlessPoolSet); unset: 32\n"
     "        static readonly int PoolTex = int.TryParse(Environment.GetEnvironmentVariable(\"TOTK_XLATE_POOL_TEX\"), out int ptx) && ptx > 32 ? ptx : 32;\n"
     "        static readonly int PoolSamp = int.TryParse(Environment.GetEnvironmentVariable(\"TOTK_XLATE_POOL_SAMP\"), out int psm) && psm > 32 ? psm : 32;\n"
     "        public bool QueryHostSupportsShaderNonUniformIndexing() => PoolTex > 32;\n"
     "        public int QuerySamplerArrayLengthFromPool() => PoolSamp;\n"),
    ("        public int QueryTextureArrayLengthFromPool() => 32;\n",
     "        public int QueryTextureArrayLengthFromPool() => PoolTex;\n")])
tr = src / 'Ryujinx.Graphics.Shader/Translation'
edit(tr / 'Optimizations/BindlessElimination.cs', [
    ("            int texturePoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromPool()) : NvnSlotArrayLength;\n",
     "            // HarmonyOS port: only the types the replay binds as whole pools take the pools' lengths (ResourceManager.BindlessPoolClass); the others keep 32\n"
     "            bool bindless = pooled && ResourceManager.BindlessPoolClass(texOp.Type) > 0;\n"
     "            int texturePoolLength = bindless ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromPool()) : pooled ? 32 : NvnSlotArrayLength;\n"),
    ("                int samplerPoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QuerySamplerArrayLengthFromPool()) : NvnSlotArrayLength;\n",
     "                int samplerPoolLength = bindless ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QuerySamplerArrayLengthFromPool()) : pooled ? 32 : NvnSlotArrayLength;\n")])
edit(tr / 'ResourceManager.cs', [
    ("        private const int DefaultLocalMemorySize = 128;\n",
     "        // HarmonyOS port (Ocarina of Time): bindless pools. TOTK_XLATE_POOL_SET (unset: off) is the descriptor set of the replay's\n"
     "        // whole-pool arrays: binding 0 the samplers, 1 2D, 2 2D array, 3 3D, 4 cube, 5 cube array textures (prepare_xlate_ns2.py)\n"
     "        public static readonly int BindlessPoolSet = int.TryParse(Environment.GetEnvironmentVariable(\"TOTK_XLATE_POOL_SET\"), out int bps) ? bps : -1;\n"
     "        private static readonly int PoolTexLength = int.TryParse(Environment.GetEnvironmentVariable(\"TOTK_XLATE_POOL_TEX\"), out int ptl) ? ptl : 32;\n"
     "        private static readonly int PoolSampLength = int.TryParse(Environment.GetEnvironmentVariable(\"TOTK_XLATE_POOL_SAMP\"), out int psl) ? psl : 32;\n\n"
     "        public static int BindlessPoolClass(SamplerType type)\n"
     "        {\n"
     "            if (BindlessPoolSet < 0 || (type & SamplerType.Multisample) != 0)\n"
     "            {\n"
     "                return -1;\n"
     "            }\n\n"
     "            bool array = (type & SamplerType.Array) != 0;\n\n"
     "            return (type & SamplerType.Mask) switch\n"
     "            {\n"
     "                SamplerType.Texture2D => array ? 2 : 1,\n"
     "                SamplerType.Texture3D => array ? -1 : 3,\n"
     "                SamplerType.TextureCube => array ? 5 : 4,\n"
     "                _ => -1,\n"
     "            };\n"
     "        }\n\n"
     "        private static int BindlessPoolBinding(SamplerType type, int arrayLength)\n"
     "        {\n"
     "            if (BindlessPoolSet < 0 || arrayLength <= 32)\n"
     "            {\n"
     "                return -1;\n"
     "            }\n\n"
     "            if (type == SamplerType.None)\n"
     "            {\n"
     "                return arrayLength == PoolSampLength ? 0 : -1;\n"
     "            }\n\n"
     "            return arrayLength == PoolTexLength ? BindlessPoolClass(type) : -1;\n"
     "        }\n\n"
     "        private const int DefaultLocalMemorySize = 128;\n"),
    ("                if (arrayLength > 1 && (setIndex = _gpuAccessor.CreateExtraSet()) >= 0)\n",
     "                int poolBinding = isImage ? -1 : BindlessPoolBinding(type, arrayLength);\n\n"
     "                if (poolBinding >= 0)\n"
     "                {\n"
     "                    // HarmonyOS port: a whole-pool array, in the replay's bindless set at its type's binding\n"
     "                    setIndex = BindlessPoolSet;\n"
     "                    binding = poolBinding;\n"
     "                }\n"
     "                else if (arrayLength > 1 && (setIndex = _gpuAccessor.CreateExtraSet()) >= 0)\n"),
    ("        private const int DefaultLocalMemorySize = 128;\n",
     "        public static int BindlessUsageWords => (PoolTexLength + 31) / 32;\n\n"
     "        private const int DefaultLocalMemorySize = 128;\n")])

# The usage bitmap (binding 6 of the bindless set): every sample through a pool array sets its texture ID's bit, read first
# so that a set bit costs one cached load. The replay reads the bitmaps back after each submission slot's fence and prepares
# only the textures the GPU sampled (a game registers far more than it draws: realizing all 18688 registered textures of
# Ocarina of Time's forest got the app killed at its memory threshold, oot-a39).
edit(shd / 'CodeGenContext.cs', [
    ("        public bool WasNonUniformAccessDeclared { get; set; }\n",
     "        public bool WasNonUniformAccessDeclared { get; set; }\n"
     "        public Instruction BindlessUsed { get; set; }   // HarmonyOS port: the bindless pools' usage bitmap (Declarations.DeclareBindlessUsage)\n")])
edit(shd / 'Declarations.cs', [
    ("            DeclareImages(context, context.Properties.Images.Values);\n",
     "            DeclareImages(context, context.Properties.Images.Values);\n"
     "            DeclareBindlessUsage(context);\n"),
    ("        private static void DeclareSamplers(CodeGenContext context, IEnumerable<TextureDefinition> samplers)\n",
     "        // HarmonyOS port: the bindless pools' usage bitmap, binding 6 of descriptor set ResourceManager.BindlessPoolSet: a bit per\n"
     "        // texture ID a sample went through (Instructions.MarkBindlessUsed), from which the replay prepares the pool's textures\n"
     "        private static void DeclareBindlessUsage(CodeGenContext context)\n"
     "        {\n"
     "            // the vertex stages may not store (Maleoon: no vertexPipelineStoresAndAtomics); textures only they sample are reported\n"
     "            // when a fragment or compute shader samples them too\n"
     "            if (context.Definitions.Stage != ShaderStage.Fragment && context.Definitions.Stage != ShaderStage.Compute)\n"
     "            {\n"
     "                return;\n"
     "            }\n\n"
     "            bool any = false;\n\n"
     "            foreach (TextureDefinition texture in context.Properties.Textures.Values)\n"
     "            {\n"
     "                if (ResourceManager.BindlessPoolSet >= 0 && texture.Set == ResourceManager.BindlessPoolSet && texture.Type != SamplerType.None)\n"
     "                {\n"
     "                    any = true;\n"
     "                }\n"
     "            }\n\n"
     "            if (!any)\n"
     "            {\n"
     "                return;\n"
     "            }\n\n"
     "            bool modern = context.Version >= 0x10400;\n"
     "            StorageClass storageClass = modern ? StorageClass.StorageBuffer : StorageClass.Uniform;\n"
     "            SpvInstruction words = context.TypeArray(context.TypeU32(), context.Constant(context.TypeU32(), ResourceManager.BindlessUsageWords), true);\n"
     "            context.Decorate(words, Decoration.ArrayStride, (LiteralInteger)4);\n"
     "            SpvInstruction block = context.TypeStruct(true, words);\n"
     "            context.Decorate(block, modern ? Decoration.Block : Decoration.BufferBlock);\n"
     "            context.MemberDecorate(block, 0, Decoration.Offset, (LiteralInteger)0);\n"
     "            SpvInstruction variable = context.Variable(context.TypePointer(storageClass, block), storageClass);\n"
     "            context.Name(variable, \"bindless_used\");\n"
     "            context.Decorate(variable, Decoration.DescriptorSet, (LiteralInteger)ResourceManager.BindlessPoolSet);\n"
     "            context.Decorate(variable, Decoration.Binding, (LiteralInteger)6);\n"
     "            context.AddGlobalVariable(variable);\n"
     "            context.BindlessUsed = variable;\n"
     "        }\n\n"
     "        private static void DeclareSamplers(CodeGenContext context, IEnumerable<TextureDefinition> samplers)\n")])
edit(shd / 'Instructions.cs', [
    ("                image = context.AccessChain(declaration.SampledImagePointerType, image, textureIndex);\n",
     "                if (context.BindlessUsed != null && texOp.GetTextureSetAndBinding().SetIndex == ResourceManager.BindlessPoolSet)\n"
     "                {\n"
     "                    MarkBindlessUsed(context, textureIndex);   // HarmonyOS port\n"
     "                }\n\n"
     "                image = context.AccessChain(declaration.SampledImagePointerType, image, textureIndex);\n"),
    ("        private static SpvInstruction GenerateSampledImageLoad(CodeGenContext context, AstTextureOperation texOp, SamplerDeclaration declaration, ref int srcIndex)\n",
     "        // HarmonyOS port: set the texture ID's bit in the bindless pools' usage bitmap (Declarations.DeclareBindlessUsage), testing it\n"
     "        // first: once the bit is set, a sample costs a cached load and no atomic\n"
     "        private static void MarkBindlessUsed(CodeGenContext context, SpvInstruction index)\n"
     "        {\n"
     "            SpvInstruction u32 = context.TypeU32();\n"
     "            SpvInstruction id = context.Bitcast(u32, index);\n"
     "            SpvInstruction word = context.ShiftRightLogical(u32, id, context.Constant(u32, 5));\n"
     "            SpvInstruction bit = context.ShiftLeftLogical(u32, context.Constant(u32, 1), context.BitwiseAnd(u32, id, context.Constant(u32, 31)));\n"
     "            StorageClass storageClass = context.Version >= 0x10400 ? StorageClass.StorageBuffer : StorageClass.Uniform;\n"
     "            SpvInstruction pointer = context.AccessChain(context.TypePointer(storageClass, u32), context.BindlessUsed, context.Constant(context.TypeS32(), 0), word);\n"
     "            SpvInstruction unset = context.IEqual(context.TypeBool(), context.BitwiseAnd(u32, context.Load(u32, pointer), bit), context.Constant(u32, 0));\n"
     "            SpvInstruction mark = context.Label();\n"
     "            SpvInstruction done = context.Label();\n"
     "            context.SelectionMerge(done, SelectionControlMask.MaskNone);\n"
     "            context.BranchConditional(unset, mark, done);\n"
     "            context.AddLabel(mark);\n"
     "            context.AtomicOr(u32, pointer, context.Constant(u32, 1), context.Constant(u32, 0), bit);\n"
     "            context.Branch(done);\n"
     "            context.AddLabel(done);\n"
     "        }\n\n"
     "        private static SpvInstruction GenerateSampledImageLoad(CodeGenContext context, AstTextureOperation texOp, SamplerDeclaration declaration, ref int srcIndex)\n")])
if not SWAP2:
  edit(shd / 'Declarations.cs', [
    ("                    context.Decorate(structType, isBuffer ? Decoration.BufferBlock : Decoration.Block);\n",
     "                    context.Decorate(structType, isBuffer && context.Version < 0x10400 ? Decoration.BufferBlock : Decoration.Block);   // HarmonyOS port: no BufferBlock from 1.4\n"),
    ("                SpvInstruction pointerType = context.TypePointer(StorageClass.Uniform, structType);\n"
     "                SpvInstruction variable = context.Variable(pointerType, StorageClass.Uniform);\n",
     "                StorageClass bufferClass = isBuffer && context.Version >= 0x10400 ? StorageClass.StorageBuffer : StorageClass.Uniform;   // HarmonyOS port: 1.4+\n"
     "                SpvInstruction pointerType = context.TypePointer(bufferClass, structType);\n"
     "                SpvInstruction variable = context.Variable(pointerType, bufferClass);\n")])

# A texel fetch with an offset read from a register (Ocarina of Time: agl's compute d17eefaff4f0bc23, the last step before the
# scene reaches the screen): the translator passed it as the image operand Offset, which Vulkan takes on gathers only
# (spirv-val VUID-RuntimeSpirv-Offset-10213, "Image Operand Offset can only be used with OpImage*Gather operations"), so every
# in-game variant of that shader was quarantined, its dispatch never ran and the picture stayed black under the HUD.
# texelFetchOffset(P, lod, o) is texelFetch(P + o, lod): the offset is added to the coordinates (an array layer gets 0).
# Constant offsets keep ConstOffset, byte for byte as before. (Swap2-Native has the same case: shaders.md "Register texel
# offsets on fetches and samples (Vulkan)".)
edit(shd / 'Instructions.cs', [(
    "            if (hasOffset)\n"
    "            {\n"
    "                operandsMask |= offsetsConst ? ImageOperandsMask.ConstOffset : ImageOperandsMask.Offset;\n"
    "                operandsList.Add(offsets[0]);\n"
    "            }\n",
    "            if (hasOffset && intCoords && !offsetsConst)\n"
    "            {\n"
    "                // HarmonyOS port: a fetch's register offset goes into its coordinates (Vulkan: a non-constant Offset on gathers only)\n"
    "                SpvInstruction fetchOffset = offsets[0];\n\n"
    "                if (pCount > coordsCount)\n"
    "                {\n"
    "                    SpvInstruction[] padded = new SpvInstruction[pCount];\n\n"
    "                    for (int index = 0; index < pCount; index++)\n"
    "                    {\n"
    "                        padded[index] = index >= coordsCount ? context.Constant(context.TypeS32(), 0) :\n"
    "                            coordsCount > 1 ? context.CompositeExtract(context.TypeS32(), fetchOffset, (SpvLiteralInteger)index) : fetchOffset;\n"
    "                    }\n\n"
    "                    fetchOffset = context.CompositeConstruct(context.TypeVector(context.TypeS32(), pCount), padded);\n"
    "                }\n\n"
    "                pCoords = context.IAdd(pCount > 1 ? context.TypeVector(context.TypeS32(), pCount) : context.TypeS32(), pCoords, fetchOffset);\n"
    "            }\n"
    "            else if (hasOffset)\n"
    "            {\n"
    "                operandsMask |= offsetsConst ? ImageOperandsMask.ConstOffset : ImageOperandsMask.Offset;\n"
    "                operandsList.Add(offsets[0]);\n"
    "            }\n")])

# the reference's Ryujinx patches that nxnative does not carry yet; publish_native.sh's loop is turned off. unidiff.py
# applies them where GNU patch -p1 would (offsets, fuzz up to 2; the same tree as Git's patch.exe gave), so the PC needs
# no patch program; a patch goes in whole or not at all and leaves no .orig / .rej
import unidiff   # noqa: E402
for p in sorted((xl / 'patches').glob('*.patch')):
    if not unidiff.apply(ryu, p, reverse=True, dry_run=True):
        print('already in nxnative:', p.name)
        continue
    failed = unidiff.apply(ryu, p)
    if failed:
        if p.name.startswith('0002-'):   # nxnative has it, extended (Optimizer.cs RemoveNode: DestHasNoUses too)
            opt = (ryu / 'src/Ryujinx.Graphics.Shader/Translation/Optimizations/Optimizer.cs').read_text(encoding='utf-8')
            assert 'DestHasNoUses(src.AsgOp)' in opt or 'IsUnused(src.AsgOp)' in opt
            print('in nxnative in a later form:', p.name)
            continue
        sys.exit(f'{p.name}: ' + '; '.join(failed))
    print('applied:', p.name)
pub = xl / 'publish_native.sh'
t = pub.read_text(encoding='utf-8')
loop = 'for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done\n'
assert t.count(loop) == 1
pub.write_bytes(t.replace(loop, ': # patches applied by prepare_xlate_ns2.py\n').encode('utf-8'))
crlf = [str(p.relative_to(out)) for p in out.rglob('*') if p.is_file() and p.suffix in TEXT and b'\r\n' in p.read_bytes()]
assert not crlf, f'CRLF left in {crlf[:5]}'
print('prepared', out)
