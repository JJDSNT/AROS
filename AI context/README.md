# Contexto de trabalho da IA

Esta pasta é a memória persistente de trabalho para a colaboração sobre o
port AArch64 do AROS. Ela pode conter:

- objetivos do projeto;
- achados técnicos e suas evidências;
- decisões e justificativas;
- trabalho concluído;
- problemas conhecidos;
- próximos passos;
- comandos úteis para reproduzir resultados.

Os documentos devem ser atualizados conforme o trabalho progride, evitando
que descobertas importantes dependam somente do histórico da conversa.

## Índice

- Este arquivo: ambiente e build do AROS para `raspi-aarch64`.
- `VC4_GALLIUM.md`: objetivo, diagnóstico e roteiro do suporte Gallium/Mesa
  para a GPU VideoCore IV.

## Contexto do build AROS para AArch64

Este documento reúne os achados iniciais sobre a compilação deste checkout do
AROS para AArch64.

## Alvo disponível

O alvo AArch64 standalone encontrado no repositório é:

```text
raspi-aarch64
```

Ele é voltado principalmente ao Raspberry Pi 3:

- arquitetura ARMv8-A;
- ajuste para Cortex-A53;
- suporte a CRC e SIMD/NEON;
- formato de objeto AArch64 ELF;
- imagem final para boot no Raspberry Pi.

Essas configurações aparecem na seção do alvo Raspberry Pi em
`configure.in`.

## Endianness do alvo atual

O alvo específico `raspi-aarch64` é **little-endian**.

Evidências no código:

- `arch/aarch64-all/include/aros/cpu.h` define `AROS_BIG_ENDIAN` como `0`;
- `arch/aarch64-native/kernel/ldscript.lds` usa
  `OUTPUT_FORMAT("elf64-littleaarch64")`;
- o alvo de distribuição é chamado internamente de
  `distfiles-raspi-aarch64le-bootimg`.

Isso não deve ser generalizado para todo o código BCM2708. A árvore
`arch/arm-native/soc/broadcom/2708/` é compartilhada com outros alvos ARM, e o
AROS já possui definições `aros-armeb` big-endian. Mesmo que não exista hoje
um alvo Raspberry Pi big-endian validado, os drivers compartilhados devem
continuar compiláveis e corretos nesse cenário.

Os periféricos BCM2708/V3D expõem registradores little-endian. Portanto:

- acessos MMIO usam `AROS_LE2LONG`/`AROS_LONG2LE`;
- essas macros são no-op em builds little-endian e fazem swap em builds
  big-endian;
- valores retornados pelas APIs do recurso ficam em byte order nativa da CPU;
- estruturas internas entre `vc4gallium.hidd` e `vc4.resource` devem usar a
  ABI nativa; apenas a fronteira com hardware recebe conversão explícita.

## Toolchain

O sistema de build possui suporte tanto para famílias GNU quanto LLVM, através
da opção:

```text
--with-toolchain=gnu
--with-toolchain=llvm
```

Para `raspi-aarch64`, a matriz de builds oficiais presente no `README.md`
mostra somente o build LLVM. Portanto, LLVM é o caminho inicial mais testado
para este alvo.

A versão LLVM padrão deste checkout está registrada em `config/llvm_def`:

```text
11.0.0
```

O build não depende apenas do Clang fornecido pela distribuição Linux. O alvo
`crosstools` baixa, modifica e compila o cross-toolchain do AROS, incluindo
LLVM, LLD, Clang, compiler-rt, libc++ e componentes relacionados.

Há infraestrutura GNU para AArch64 em `tools/crosstools/gnu`, mas ela não
aparece como configuração oficial ativa para `raspi-aarch64` na matriz deste
checkout. Deve ser considerada uma alternativa menos testada.

## Dependências sugeridas no Ubuntu

O host observado durante esta análise usa Ubuntu 24.04.

Uma lista prática baseada nos scripts de CI e nos scripts auxiliares do
repositório é:

```bash
sudo apt update
sudo apt install \
  build-essential gcc g++ make cmake gawk bison flex \
  autoconf automake python3-mako unzip \
  netpbm libpng-dev zlib1g-dev libxcursor-dev \
  libgl1-mesa-dev libasound2-dev liblzo2-dev \
  libxxf86vm-dev libx11-dev libxext-dev \
  libswitch-perl gperf ccache \
  genisoimage mtools xorriso nasm \
  wget bzip2 xz-utils jlha-utils
```

Na verificação inicial desta máquina, estavam ausentes pelo menos:

```text
autoconf
automake
python3-mako
genisoimage
mtools
```

Clang, Clang++, LLD e as ferramentas LLVM do sistema também não estavam
instalados. Isso não significa necessariamente que precisem ser usados como o
toolchain final, pois o AROS constrói seu próprio cross-toolchain, mas pode ser
útil instalá-los caso o processo de bootstrap venha a exigi-los.

## Build separado das fontes

É recomendado manter os arquivos gerados em um diretório separado. Isso:

- evita misturar objetos e configurações com o código-fonte;
- facilita limpar e refazer o build;
- permite manter builds de vários alvos simultaneamente;
- reduz o risco de tratar arquivos gerados como alterações no Git.

Dentro das permissões atuais do workspace, o layout pode ser:

```text
/home/jaime/AROS/
├── build-aarch64/
├── toolchain-aarch64/
├── arch/
├── compiler/
└── ...
```

Mesmo estando fisicamente sob o repositório, `build-aarch64` continua sendo
uma árvore de build separada da árvore de fontes.

## Configuração e compilação validada

```bash
git submodule update --init --recursive

mkdir -p /tmp/aros-build-aarch64-core
cd /tmp/aros-build-aarch64-core

/home/jaime/AROS/configure \
  --target=raspi-aarch64 \
  --with-toolchain=llvm \
  --with-llvm-version=11.0.0 \
  --with-aros-toolchain-install=/home/jaime/AROS/toolchain-aarch64 \
  --with-aros-toolchain=yes

make -j6 features
CMAKE_BUILD_PARALLEL_LEVEL=6 make -j6
CMAKE_BUILD_PARALLEL_LEVEL=6 make -j6 boot-distfiles
CMAKE_BUILD_PARALLEL_LEVEL=6 make -j6 distfiles
```

O toolchain LLVM 11 já foi construído e instalado em
`/home/jaime/AROS/toolchain-aarch64`. A árvore de build do sistema está em
`/tmp/aros-build-aarch64-core`, fora da árvore de fontes porque o scanner do
MetaMake também percorre subdiretórios da fonte e pode confundir arquivos
gerados com fontes reais.

A ordem canônica documentada pelo CI é:

1. `make`;
2. `make boot-distfiles`;
3. `make distfiles`.

`features` deve existir antes de chamar diretamente alvos do MetaMake. Sem
`bin/raspi-aarch64/gen/config/compiler.cfg`, `CFLAGS_IQUOTE` fica vazio e os
diretórios de include são passados ao Clang como argumentos soltos.

`CMAKE_BUILD_PARALLEL_LEVEL=6` é necessário porque alguns sub-builds CMake não
herdam o jobserver do GNU Make e, sem essa variável, avisam
`jobserver unavailable` e executam serialmente.

## Saída esperada

O código de boot gerou a imagem:

```text
/tmp/aros-build-aarch64-core/bin/raspi-aarch64/AROS/aros-aarch64-raspi.img
```

Artefatos validados:

```text
AROS/aros-aarch64-raspi.img                 548.8 KiB (imagem de boot)
gen/rom/boot/core.elf                      246.8 KiB (ELF64 LSB AArch64)
AROS/Devs/Drivers/softpipe.hidd               1.7 MiB (ELF64 LSB AArch64)
```

## Estado inicial do repositório

No início da análise:

- branch: `master`, acompanhando `origin/master`;
- working tree: limpa;
- commit: `d0370bd757`;
- submódulos Git: inicialmente não inicializados; agora inicializados;
- não havia diretório `build/`.

O build completo precisa dos submódulos. Um exemplo é
`developer/debug/test/crt/posixc/posix.1`, que contém
`scanf-examples.c`; sem ele, o alvo principal falha nos testes POSIX.

## Estado em 28/07/2026

- branch: `feature/aarch64-vc4-gallium`;
- toolchain LLVM AROS AArch64: concluído;
- detecção `features`: concluída;
- build principal (`make -j6`): concluído com código 0;
- `boot-distfiles`: concluído com código 0 (sem trabalho adicional para este
  alvo);
- `distfiles`: concluído com código 0;
- kernel `core.elf`: gerado e validado como ELF64 little-endian AArch64;
- Mesa 20.0.8: baixado e com patch AROS aplicado na árvore de build;
- `softpipe.hidd`: gerado e validado como ELF64 little-endian AArch64;
- imagem `aros-aarch64-raspi.img`: gerada;
- dependência adicional descoberta e instalada: `unzip`;
- pendência de empacotamento: os blobs opcionais de Wi-Fi Broadcom não foram
  obtidos; o build apenas emitiu avisos e concluiu normalmente;
- próximo trabalho: inventariar as dependências exatas do Gallium VC4 na
  árvore do Mesa corrigida para AROS;
- alvo explícito `hidd-vc4gallium-skeleton`: compilado com sucesso;
- `vc4gallium.hidd` inicial: validado como ELF64 little-endian AArch64, mas
  ainda não incluído na distribuição porque não cria um `pipe_screen`;
- alvo explícito `kernel-vc4-bcm2708-skeleton`: compilado com sucesso;
- `vc4.resource` inicial: validado como ELF64 little-endian AArch64 e com
  símbolo `VC4GetParam`, mas ainda não incluído no ROM da distribuição;
- ativação QPU/V3D via property tag `ENABLE_QPU`: implementada com falha
  fechada e compilada, ainda pendente de teste no Raspberry Pi real;
- caminho de sondagem `vc4gallium.hidd -> vc4.resource -> IDENT*`: integrado
  e compilado; ainda preserva o fallback porque não cria `pipe_screen`;
- BOs iniciais: criação, mapeamento, sincronização de cache e liberação
  implementadas no `vc4.resource`, compiladas e pendentes de teste real.
- fachada DRM VC4 mínima: `GET_PARAM`, criação de BO comum/shader, mapeamento
  e fechamento traduzidos para `vc4.resource`; compilada dentro do
  `vc4gallium.hidd`, ainda pendente de conexão ao `vc4_bufmgr.c` do Mesa.
- backend AROS de `vc4_bufmgr`: API pública de BO do Mesa implementada sem
  libdrm/mmap, compilada contra os headers do Mesa 20.0.8 e ligada ao
  `vc4gallium.hidd`; PRIME/flink/importação e espera real ainda ausentes.
- `pipe_screen` de sondagem: ciclo de criação/destruição Gallium implementado,
  com validação V3D 2.1/2.6; não anuncia aceleração e cria apenas contextos de
  transferência, sem renderização ou submissão.
- recursos lineares iniciais: criação/destruição de `PIPE_BUFFER` respaldado
  por BO e helpers de mapeamento com sincronização explícita de cache.
- contexto de transferência: `pipe_context` mínimo com `transfer_map/unmap`
  para buffers e texturas 2D lineares, referências Gallium e validação de
  intervalos; ainda sem callbacks de renderização ou submissão.
- texturas 2D lineares de nível zero: stride/tamanho calculados pelos helpers
  oficiais de formato do Mesa, transferências por região e proteção contra
  overflow; não anunciadas para sampling, render target ou scanout.
- autoteste opcional: round-trip de um padrão não alinhado em BO de mais de
  16 KiB através dos callbacks Gallium e sincronização de cache; compilado nos
  modos ligado/desligado, ficando desligado por padrão até teste em hardware.
- tiling VC4 em CPU: código upstream LT/T ligado por wrappers AROS, incluindo
  caminho SIMD AArch64; autoteste opcional ampliado com round-trips LT 13×11
  e T 37×35, compilado mas ainda pendente de execução no Raspberry Pi.
- recursos 2D tiled: escolha automática LT/T, BO padded e staging linear em
  `transfer_map/unmap`; autoteste Gallium cria e faz round-trip de uma textura
  T, ainda pendente de execução em hardware.
- miptrees 2D: até 12 slices LT/T, níveis menores armazenados primeiro e nível
  zero alinhado a 4 KiB; transferências selecionam o slice correto e o
  autoteste compila round-trip independente para quatro níveis.
- fronteira `SUBMIT_CL`: ABI de 176 bytes reproduzida, limites/pointers/handles
  e superfícies validados pelo `vc4.resource`; a fachada retorna explicitamente
  “não implementado” após sucesso, sem tocar no V3D.
- snapshots de submissão: bin CL, shader records, uniforms e handles são
  copiados para até 32 MiB de memória do recurso antes da resolução, sem
  reter ponteiros do caller.
- decoder estrutural de bin CL: somente o subconjunto VC4 permitido é aceito,
  com comprimentos, ordem de configuração/início, terminação obrigatória,
  referências de BO/shader e leitura little-endian explícita validadas antes
  de qualquer futura execução.
- shader records: formatos normal/extended, tabelas de hindices, offsets dos
  três shaders e limites de todos os atributos são validados contra os BOs;
  BOs de shader e de dados agora possuem tipos distintos. A validação das
  instruções QPU e dos uniforms continua pendente e a execução segue bloqueada.
- QPU preliminar: tamanho lógico do shader, instruções little-endian, sinais,
  terminação, branches e destinos de escrita imediatamente perigosos são
  checados. A análise completa de data-flow continua pendente, portanto esta
  etapa ainda não autoriza execução.
- uniforms/threading QPU: leituras diretas produzem um limite inferior
  confrontado com o stream de uniforms; regras de delay entre thread switches
  e uso da metade superior dos registradores também são verificadas. Resets do
  endereço de uniforms e o modo TMU direto ainda não fazem parte da prova.
- TMU conservador: sequências das duas unidades, limite de quatro parâmetros,
  hindices de textura e consumo de uniforms são rastreados. TMU direto e sua
  combinação com branches ficam rejeitados até a análise de clamps/basic
  blocks estar completa.
- hindices de textura: a organização `[handles][uniform data]` de cada shader
  é percorrida no snapshot, cada referência é resolvida e shader BOs são
  proibidos como textura. A interpretação completa de P0-P3 e os bounds da
  imagem continuam pendentes.
- P0/P1 inicial: os offsets dos parâmetros são reconstruídos diretamente dos
  writes TMU, P0/P1 são obrigatórios e o endereço-base P0 deve cair dentro do
  texture BO. Essa reconstrução alimenta o cálculo de bounds subsequente.
- bounds da textura base: formato, largura, altura, ETC1 e layouts linear/LT/T
  determinam a área alinhada, verificada integralmente contra o BO.
- mipmaps/cube maps: níveis menores são caminhados para trás com transição
  T→LT e proteção contra underflow; cube stride em P2/P3 cobre todas as seis
  faces e não pode estar ausente nem duplicado.
- index buffers: offset, quantidade e índice U8/U16 são validados em 64 bits
  contra o BO selecionado por `GEM_HANDLES`. Somas futuras com o endereço de
  barramento também são verificadas para index, vertex e texture BOs.
- bin CL compactada: uma segunda cópia privada omite `GEM_HANDLES`, recebe a
  relocation do index buffer e neutraliza endereços de tile/shader ainda não
  resolvidos. Ela é descartada sem execução ao final da validação.
- uniforms compactados: hindices de textura são removidos, apenas os words
  consumidos são copiados e P0 recebe a relocation do texture BO. A cópia
  ainda não possui endereço de barramento e é descartada sem execução.
- shader records compactados: tabelas de hindices são removidas, records ficam
  alinhados a 16 bytes e shaders/atributos recebem relocations. Ponteiros para
  uniforms permanecem zerados até existir um BO GPU interno.
- BO de staging: bin CL, shader records e uniforms compactados são alinhados,
  copiados para um único BO GPU-visible, sincronizados e imediatamente
  liberados. Nenhum registrador V3D é tocado.
- referências cruzadas: `GL_SHADER_STATE` e os três ponteiros de uniforms são
  preenchidos dentro do BO usando offsets registrados durante a validação,
  com proteção contra overflow de endereço de barramento.
- tile BO: state e allocation são dimensionados pela grade de tiles, separados
  em 4 KiB e ligados ao `TILE_BINNING_MODE_CONFIG`; o BO é zerado e liberado
  sem execução.
- planejamento RCL: descritores ausentes/presentes, faixa da grade e stores
  obrigatórios são validados; o tamanho exato por combinação de loads/stores
  e quantidade de tiles é calculado em 64 bits, ainda sem emissão.
- RCL color mínima: render config, coordenadas, espera, branches por tile e
  store/EOF são emitidos em um BO próprio para o caminho color-write simples;
  o BO é sincronizado e liberado sem execução.

## Referências locais consultadas

- `README.md`
- `configure.in`
- `config/llvm_def`
- `arch/aarch64-all/include/aros/cpu.h`
- `arch/aarch64-native/kernel/ldscript.lds`
- `arch/aarch64-raspi/boot/mmakefile.src`
- `tools/crosstools/gnu/mmakefile.src`
- `tools/crosstools/llvm/mmakefile.src`
- `scripts/azure/templates/steps-prepare-host.yml`
- `scripts/azure/templates/steps-toolchain.yml`
- `scripts/azure/templates/steps-core.yml`
- `scripts/gimmearos.sh`
