# Port Gallium/Mesa VC4 para AROS AArch64

## Objetivo principal

Portar o driver Gallium VC4 do Mesa para o AROS no Raspberry Pi 3 AArch64,
permitindo aceleração 3D real pela GPU VideoCore IV e tornando o suporte
gráfico mais completo que o framebuffer/2D atual.

O alvo inicial é:

```text
raspi-aarch64
```

O hardware inicial é Raspberry Pi 3/BCM2837. Raspberry Pi 4 e Pi 5 não fazem
parte deste primeiro escopo.

## Situação atual

### O que existe

O `vc4gfx.hidd` é incluído explicitamente no pacote AArch64 e oferece:

- framebuffer controlado pelo firmware;
- modos HDMI e SDTV;
- framebuffer de 32 bits;
- cursor de hardware;
- mudança de resolução;
- DMA para operações 2D grandes;
- fallback por CPU;
- double buffering e page flipping quando suportados pelo framebuffer;
- integração parcial preparada para um futuro `vc4gallium.hidd`.

O código fica em:

```text
arch/arm-native/soc/broadcom/2708/hidd/vc4gfx/
```

Apesar do nome do diretório, ele é compartilhado pelos ports ARM e AArch64.

O port AArch64 e a adaptação dos drivers BCM2708 foram incorporados em
24/07/2026. Há comentários no código relatando problemas observados em
Raspberry Pi 3 real, especialmente relacionados à coerência de cache.

### O que não existe

Não há implementação de `vc4gallium.hidd` neste checkout.

O `vc4gfx.hidd` tenta carregar:

```text
vc4gallium.hidd
hidd.gallium.vc4
```

mas esses componentes ainda não estão presentes. Quando não são encontrados,
a infraestrutura pode usar o renderer por software `softpipe`.

Portanto, o driver atual não programa a parte 3D/QPU da GPU VC4. Ele é
principalmente um driver de display e aceleração 2D via DMA.

## Mesa usado pelo AROS

A versão padrão é:

```text
Mesa 20.0.8
```

Ela é definida em:

```text
workbench/libs/mesa/mesa.cfg
```

O build baixa o arquivo oficial do Mesa e aplica:

```text
workbench/libs/mesa/mesa-20.0.8-aros.diff
```

O AROS já possui:

- `mesa3dgl.library`;
- a interface base `gallium.hidd`;
- integração Gallium/Mesa;
- `softpipe.hidd`;
- exemplos de integração em Nouveau, Intel GMA e VMware SVGA.

O Mesa 20.0.8 já contém o driver Gallium VC4, mas o AROS ainda não fornece o
winsys e os serviços de kernel esperados por esse driver.

## Principal lacuna arquitetural

O driver Gallium VC4 normalmente depende de serviços DRM/libdrm:

- consulta de parâmetros da GPU;
- criação, mapeamento e destruição de buffer objects;
- endereçamento dos buffers pela GPU;
- submissão de command lists;
- relocations;
- sincronização e espera por conclusão;
- interrupções;
- timeout e reset de GPU;
- manutenção de cache e memória.

O framebuffer, `mbox.resource` e `dma.resource` atuais não implementam essa
interface de renderização.

## Arquitetura proposta

```text
Aplicação OpenGL
    ↓
mesa3dgl.library
    ↓
Gallium VC4 do Mesa
    ↓
vc4gallium.hidd
    ↓
compatibilidade libdrm_vc4 / winsys VC4
    ↓
vc4.resource
    ↓
V3D, memória, interrupções e submissão de comandos
```

### `vc4gallium.hidd`

Deve implementar pelo menos:

- `Root.New`;
- `Hidd_Gallium.CreatePipeScreen`;
- `Hidd_Gallium.DestroyPipeScreen`;
- `Hidd_Gallium.DisplayResource`.

O ponto de integração já está preparado em `vc4gfx_hiddclass.c`.

### `vc4.resource`

Uma API inicial deve fornecer operações conceitualmente equivalentes a:

```text
VC4GetParam
VC4CreateBO
VC4MapBO
VC4FreeBO
VC4SubmitCL
VC4WaitSeqno
```

Também serão necessários:

- detecção do bloco V3D;
- alocação em região endereçável pelo VC4;
- tradução CPU physical ↔ VC4 bus address;
- manutenção explícita de cache;
- interrupção de conclusão;
- timeout;
- reset e recuperação da GPU;
- serialização das submissões.

## Estratégia de compatibilidade

É preferível manter o driver Gallium VC4 próximo ao código original do Mesa.

A abordagem recomendada é implementar o subconjunto de `libdrm_vc4` realmente
usado pelo Mesa 20.0.8 e traduzi-lo para `vc4.resource`. Isso tende a reduzir
o patch local e facilitar futuras atualizações do Mesa.

Uma adaptação direta do winsys VC4 ao AROS continua sendo uma alternativa,
caso a camada de compatibilidade libdrm se revele desnecessariamente grande.

## Apresentação inicial

O primeiro marco não precisa ser zero-copy:

```text
Gallium VC4 renderiza em BO
    ↓
cópia para bitmap/framebuffer do vc4gfx.hidd
    ↓
imagem exibida
```

Depois de validar a renderização, pode-se integrar:

```text
BO renderizável e compatível com scanout
    ↓
page flip
```

O `vc4gfx.hidd` já contém infraestrutura de page flipping preparada para essa
integração futura.

## Limitações e riscos já identificados

### Cache e memória da GPU

Bitmaps off-screen em memória da GPU estão desativados:

```c
#define VC4_GPUBM_ENABLE 0
```

O motivo é incoerência entre o mapeamento cached da CPU e o acesso uncached
feito pelo DMA. Em hardware real isso gerou linhas de pixels antigos.

Antes de usar BOs para renderização, a política de cache deve ser definida e
testada cuidadosamente.

### AArch64 e endereços VC4

Embora a CPU seja de 64 bits, o VC4 do Pi 3 usa endereçamento/bus addresses de
32 bits. Os buffers da GPU devem permanecer em uma região física acessível
pelo dispositivo. Conversões silenciosas de ponteiros AArch64 para `ULONG`
só são válidas quando essa restrição foi garantida pela alocação.

### NEON

As rotinas NEON do `vc4gfx.hidd` usam assembly AArch32. No AArch64 elas caem
em implementações C escalares. Isso afeta desempenho de cópia/fill, mas não
bloqueia o port Gallium.

### Robustez de command lists

Uma primeira versão pode confiar nas command lists produzidas pelo Mesa.
Uma implementação robusta deverá considerar validação, limites de memória,
timeouts e recuperação após GPU hang.

## Plano de trabalho

1. Concluir um build funcional de `raspi-aarch64`.
2. Confirmar Mesa 20.0.8 e `softpipe.hidd` no AArch64.
3. Obter a árvore do Mesa já corrigida pelo patch AROS.
4. Inventariar exatamente as dependências do driver e winsys VC4.
5. Especificar a API mínima de `vc4.resource`.
6. Criar um `vc4gallium.hidd` mínimo que carregue e registre sua classe.
7. Implementar detecção V3D e `GET_PARAM`.
8. Implementar criação, mapeamento e destruição de BOs.
9. Implementar submissão e espera por command lists.
10. Adicionar interrupção, timeout e reset.
11. Criar o `pipe_screen` VC4.
12. Renderizar off-screen e copiar para o framebuffer.
13. Integrar page flipping e, se possível, zero-copy.
14. Avaliar atualização do Mesa somente após o backend básico funcionar.

## Próximo passo

O build completo foi concluído, e a árvore corrigida do Mesa está disponível
em:

```text
/tmp/aros-build-aarch64-core/bin/raspi-aarch64/Ports/mesa/mesa-20.0.8
```

Produzir agora um inventário de:

- fontes VC4 necessárias;
- símbolos externos;
- ioctls e estruturas DRM usados;
- dependências NIR/compiler;
- dependências de `libdrm_vc4`;
- mudanças mínimas no sistema de build AROS.

Esse inventário será a base para definir `vc4.resource` sem portar partes
desnecessárias do DRM Linux.

## Inventário inicial do Mesa 20.0.8

### Fontes do driver

O `src/gallium/drivers/vc4/meson.build` lista cerca de 60 arquivos. Os grupos
principais são:

- contexto, estado, draw, jobs e emissão de command lists:
  `vc4_context`, `vc4_state`, `vc4_draw`, `vc4_job`, `vc4_emit`, `vc4_cl`;
- gerenciamento de buffers e recursos:
  `vc4_bufmgr`, `vc4_resource`, `vc4_tiling`;
- compilador VC4:
  `vc4_program`, `vc4_qir*`, `vc4_qpu*`, `vc4_opt*`,
  `vc4_register_allocate`, `vc4_uniforms`;
- integração Gallium:
  `vc4_screen`, `vc4_query`, `vc4_fence`, `vc4_formats`, `vc4_blit`;
- validação/simulador:
  `kernel/vc4_*` e `vc4_simulator`.

No AArch64, `vc4_tiling_lt_neon.c` não é selecionado pelo Meson: ele só entra
quando `cpu_family()` é `arm`, não `aarch64`. Isso coincide com a limitação
NEON já observada no `vc4gfx.hidd`.

Além do driver, são necessárias as bibliotecas comuns já parcialmente
compiladas pelo AROS:

- Gallium auxiliary;
- NIR e compiler;
- Mesa util;
- `src/broadcom/cle` e `src/broadcom/common`/V3D XML pack.

### ABI DRM usada diretamente

Fora do simulador, o driver chama `drmIoctl()` através de `vc4_ioctl()`. Foram
encontradas as seguintes operações:

```text
DRM_IOCTL_VC4_GET_PARAM
DRM_IOCTL_VC4_CREATE_BO
DRM_IOCTL_VC4_CREATE_SHADER_BO
DRM_IOCTL_VC4_MMAP_BO
DRM_IOCTL_VC4_WAIT_BO
DRM_IOCTL_VC4_WAIT_SEQNO
DRM_IOCTL_VC4_SUBMIT_CL
DRM_IOCTL_VC4_GET_TILING
DRM_IOCTL_VC4_SET_TILING
DRM_IOCTL_VC4_GEM_MADVISE
DRM_IOCTL_VC4_LABEL_BO
DRM_IOCTL_VC4_PERFMON_CREATE
DRM_IOCTL_VC4_PERFMON_DESTROY
DRM_IOCTL_VC4_PERFMON_GET_VALUES
DRM_IOCTL_GEM_CLOSE
DRM_IOCTL_GEM_OPEN
DRM_IOCTL_GEM_FLINK
DRM_IOCTL_GEM_PRIME_FD_TO_HANDLE
```

Também aparecem funções libdrm para PRIME, syncobj e render-only. Para o
primeiro protótipo AROS, compartilhamento PRIME/flink, syncobj, perfmon,
render-only scanout e madvise podem ser desativados ou implementados como
capacidades ausentes. O caminho mínimo precisa de:

1. `GET_PARAM` (`V3D_IDENT0`, `V3D_IDENT1` e flags de capacidade);
2. `CREATE_BO`, `CREATE_SHADER_BO`, `MMAP_BO` e fechamento;
3. `GET_TILING`/`SET_TILING`;
4. `SUBMIT_CL`;
5. `WAIT_BO` e/ou `WAIT_SEQNO`.

`SUBMIT_CL` recebe ponteiros AArch64 para as command lists, shader records e
uniforms, além da lista de handles de BO. A camada AROS deverá copiá-los e
validá-los antes da submissão; não basta escrever os endereços diretamente
nos registradores V3D.

### Infraestrutura reutilizável no AROS

O checkout não tem uma libdrm genérica, mas o Nouveau contém uma adaptação
local em:

```text
workbench/hidds/nouveau/drm/libdrm/arosdrm.c
workbench/hidds/nouveau/drm/libdrm/arosdrm.h
```

Ela serve como referência para uma pequena fachada `drmIoctl`, mas é
específica ao Nouveau e não deve virar dependência direta do VC4.

O padrão de build mais próximo é `workbench/hidds/softpipe/mmakefile.src`:
ele consome o `Makefile.sources` original do Mesa, cria uma biblioteca
estática do driver e depois a liga ao módulo HIDD. Um futuro
`workbench/hidds/vc4/mmakefile.src` pode seguir esse modelo, adicionando as
fontes Broadcom, a fachada DRM VC4 e o backend para `vc4.resource`.

### Decisão para o primeiro marco

Manter as estruturas UAPI de `drm-uapi/vc4_drm.h` e substituir somente a
função de transporte é o caminho de menor alteração no Mesa:

```text
vc4_ioctl(fd, request, arg)
    -> vc4.resource/VC4Ioctl(request, arg)
```

Isso preserva o driver upstream e permite implementar a API incrementalmente.
O descritor `fd` pode ser ignorado ou substituído por um objeto privado no
port AROS. Exportação de buffers e sincronização entre processos ficam fora
do primeiro marco.

## Implementação iniciada

Foi criado um esqueleto em:

```text
workbench/hidds/vc4/
```

Ele contém:

- classe OOP com o ID exato `hidd.gallium.vc4`, esperado pelo `vc4gfx.hidd`;
- validação de `GALLIUM_INTERFACE_VERSION`;
- inicialização e liberação do attr base Gallium;
- métodos `Root.New`, `Root.Dispose`, `Root.Get`,
  `Hidd_Gallium.CreatePipeScreen` e `Hidd_Gallium.DisplayResource`;
- alvo MetaMake explícito `hidd-vc4gallium-skeleton`.

O alvo foi compilado com sucesso:

```text
/tmp/aros-build-aarch64-core/bin/raspi-aarch64/AROS/Devs/Drivers/vc4gallium.hidd
ELF 64-bit LSB relocatable, ARM aarch64
18.7 KiB
```

Por enquanto `CreatePipeScreen` retorna `NULL`. O módulo não foi ligado ao
alvo agregado `workbench-hidds` nem uma nova imagem foi empacotada com ele.
Essa escolha evita anunciar aceleração funcional antes da existência do
transporte DRM/`vc4.resource`.

O próximo incremento deve definir uma fachada de transporte com uma API
testável e implementar primeiro `GET_PARAM`, sem ainda compilar todas as
fontes do driver VC4.

## `vc4.resource` inicial

Foi criado em:

```text
arch/arm-native/soc/broadcom/2708/vc4/
```

O recurso implementa inicialmente:

```c
int VC4GetParam(unsigned int param, uint64_t *value);
```

Parâmetros implementados:

- leitura de `V3D_IDENT0`, `V3D_IDENT1` e `V3D_IDENT2`, convertendo do
  byte order little-endian do MMIO para o byte order nativo da CPU;
- capacidades opcionais conhecidas retornam sucesso com valor zero;
- parâmetros desconhecidos e ponteiro de saída nulo retornam erro.

A base MMIO é obtida com `KrnGetSystemAttr(KATTR_PeripheralBase)`. Todos os
cálculos de endereço usam `uintptr_t`, evitando truncar ponteiros AArch64 para
`ULONG`.

O uso de `AROS_LE2LONG` é necessário mesmo no alvo atual, onde vira no-op.
Este código reside numa árvore compartilhada com ARM e deve continuar correto
num possível build big-endian. A conversão pertence ao helper de MMIO, não à
API `VC4GetParam`: callers sempre recebem `uint64_t` em ordem nativa.

O recurso foi compilado pelo alvo explícito
`kernel-vc4-bcm2708-skeleton`:

```text
/tmp/aros-build-aarch64-core/bin/raspi-aarch64/AROS/Devs/vc4.resource
ELF 64-bit LSB relocatable, ARM aarch64
10.0 KiB
```

O símbolo gerado `Vc4_1_VC4GetParam` está definido e não há símbolos globais
não resolvidos no módulo. Ele ainda não foi adicionado a
`kernel-package-raspi-aarch64`.

### Ativação do QPU/V3D

Foi adicionada a property tag de firmware:

```text
VCTAG_ENABLEQPU = 0x00030012
```

Antes da primeira leitura de `IDENT*`, `VC4GetParam` agora:

1. abre e reutiliza `mbox.resource`;
2. serializa a inicialização com um semáforo próprio;
3. envia `ENABLE_QPU=1` pelo property channel;
4. valida a resposta global, a resposta da tag e o valor devolvido;
5. só acessa o MMIO V3D após sucesso;
6. memoriza sucesso ou falha para não repetir transações de firmware.

Se a ativação falhar, o recurso retorna erro sem tocar nos registradores V3D.
O código compila, mas esse fluxo ainda precisa ser validado em Raspberry Pi
real. O módulo resultante passou de 10.0 para 11.3 KiB.

### Integração com `vc4gallium.hidd`

O HIDD agora abre `vc4.resource` durante sua inicialização. Se o recurso não
estiver disponível, a abertura de `vc4gallium.hidd` falha e o caminho
existente do `vc4gfx.hidd` pode manter o fallback por software.

Quando `CreatePipeScreen` é chamado, o esqueleto consulta:

```text
VC4_PARAM_V3D_IDENT0
VC4_PARAM_V3D_IDENT1
VC4_PARAM_V3D_IDENT2
```

Com debug habilitado, os três valores são registrados. Isso forma um smoke
test completo:

```text
vc4gfx.hidd
  -> vc4gallium.hidd
     -> vc4.resource
        -> mbox.resource / ENABLE_QPU
        -> V3D MMIO / IDENT*
```

Mesmo após uma sondagem bem-sucedida, `CreatePipeScreen` continua retornando
`NULL`; naquele marco, BOs, submissão e um `pipe_screen` real ainda não
existiam. O HIDD recompilado é ELF64 AArch64 e tem 19.4 KiB.

## Buffer objects iniciais

O `vc4.resource` agora exporta:

```c
int VC4CreateBO(uint32_t size, uint32_t alignment, uint32_t flags,
                uint32_t *handle);
int VC4MapBO(uint32_t handle, void **cpu_address,
             uint32_t *bus_address, uint32_t *size);
int VC4FreeBO(uint32_t handle);
int VC4SyncBO(uint32_t handle, uint32_t offset, uint32_t length,
              uint32_t direction);
```

Implementação:

- `ALLOCMEM` e `LOCKMEM` pelo firmware;
- memória `VCMEM_DIRECT`, zerada por padrão;
- tamanhos arredondados para 4 KiB com verificação de overflow;
- alinhamento configurável, validado como potência de dois;
- handles AROS separados dos handles opacos do firmware;
- tabela de BOs serializada por semáforo;
- endereço de bus preservado com os alias bits do VideoCore;
- endereço CPU derivado somente após validar o alias retornado;
- `UNLOCKMEM` e `FREEMEM` com confirmação antes de remover o BO;
- validação de handles, ponteiros e limites de mapeamento/cache.

`VC4SyncBO` trata a memória mapeada como cached:

```text
VC4_SYNC_CPU_TO_GPU -> CACRF_ClearD
VC4_SYNC_GPU_TO_CPU -> CACRF_InvalidateD
```

Isso é deliberado: mascarar o endereço de bus produz um endereço físico
mapeado pelo ARM, mas não garante um mapping uncached. Toda futura submissão
deve limpar os BOs lidos pela GPU, e toda leitura CPU após escrita da GPU deve
invalidá-los.

O módulo compila com os cinco vetores definidos, sem símbolos globais não
resolvidos, e agora tem 14.2 KiB. As operações de firmware ainda precisam de
teste em hardware real.

## Fachada DRM mínima para o Mesa

Foi adicionada ao `vc4gallium.hidd` uma fachada semântica em:

```text
workbench/hidds/vc4/vc4_drm_compat.h
workbench/hidds/vc4/vc4_drm_compat.c
```

Ela preserva o layout das estruturas UAPI usadas pelo Mesa 20.0.8, mas não
reproduz os números codificados de ioctl do Unix. O AROS não fornece
`sys/ioccom.h`, e a codificação não traz benefício quando o transporte termina
diretamente em `vc4.resource`.

O dispatcher `vc4_drm_ioctl()` implementa atualmente:

```text
VC4_DRM_GET_PARAM
VC4_DRM_CREATE_BO
VC4_DRM_CREATE_SHADER_BO
VC4_DRM_MMAP_BO
VC4_DRM_GEM_CLOSE
```

As operações são traduzidas para `VC4GetParam`, `VC4CreateBO`, `VC4MapBO`,
`VC4SyncBO` e `VC4FreeBO`. `CREATE_SHADER_BO` copia o shader para o BO e faz
sincronização `CPU_TO_GPU`.

Em `MMAP_BO`, o campo de 64 bits `offset` recebe diretamente o ponteiro AROS.
Quando `vc4_bufmgr.c` for integrado, o caminho AROS deverá consumir esse
ponteiro sem chamar `mmap()`. PRIME, flink, render-only, madvise, tiling,
submissão e espera continuam fora desta primeira fachada.

O alvo `hidd-vc4gallium-skeleton` foi recompilado com sucesso. O módulo
resultante é ELF64 little-endian AArch64 e tem 20.6 KiB. A fachada já está
ligada ao módulo, mas ainda não foi conectada ao `vc4_bufmgr.c` do Mesa.

### Backend AROS do buffer manager

Foi criado:

```text
workbench/hidds/vc4/vc4_bufmgr_aros.c
```

Esse arquivo implementa a API pública de `vc4_bufmgr.h` do Mesa 20.0.8 usando
`vc4_drm_ioctl()`. Ele é compilado contra os headers reais da árvore Mesa
corrigida pelo AROS e fornece:

- BO comum e BO de shader;
- referência e liberação;
- mapeamento direto no address space do AROS;
- contadores de BO do `vc4_screen`;
- stubs explícitos de “não suportado” para PRIME, flink e importação por nome.

O primeiro backend não mantém cache de BOs: todos são privados e são
liberados na última referência. Isso reduz o estado compartilhado enquanto
submissão e sincronização ainda não existem. `vc4_bo_wait()` e
`vc4_wait_seqno()` retornam sucesso imediatamente apenas porque ainda não há
um caminho de submissão que possa deixar a GPU trabalhando; esse
comportamento deve ser removido junto com a implementação de `SUBMIT_CL`.

O código evita `mmap()` e libdrm. O valor devolvido por `MMAP_BO` é convertido
diretamente de `uint64_t` para `IPTR`, preservando ponteiros AArch64. Os
headers e estruturas permanecem em byte order nativa; somente MMIO e
protocolos de firmware recebem conversões little-endian.

O objeto `vc4_bufmgr_aros.o` e o HIDD foram compilados com sucesso. Os símbolos
`vc4_bo_alloc`, `vc4_bo_alloc_shader`, `vc4_bo_map`, `vc4_bo_wait` e
`vc4_bufmgr_destroy` estão definidos no módulo. O `vc4gallium.hidd` passou de
20.6 para 22.7 KiB.

## `pipe_screen` de sondagem

Foi criado um primeiro `pipe_screen` AROS em:

```text
workbench/hidds/vc4/vc4_screen_aros.c
workbench/hidds/vc4/vc4_screen_aros.h
```

Ele não é ainda um screen de renderização. Sua função é validar com segurança
o ciclo de vida Gallium/HIDD e a identificação do hardware:

- consulta `IDENT0` e `IDENT1` através de `vc4.resource`;
- aceita somente V3D 2.1 ou 2.6, as revisões aceitas pelo Mesa VC4 20.0.8;
- fornece nome, fabricante e identificação básica;
- informa `PIPE_CAP_ACCELERATED = 0`;
- anuncia buffers e armazenamento 2D linear;
- cria somente um contexto de transferência, sem renderização;
- possui destruição explícita.

`CreatePipeScreen` agora cria e memoriza esse objeto depois de uma sondagem
bem-sucedida. Foi implementado também o método HIDD `DestroyPipeScreen`, e
`Root.Dispose` destrói o screen caso o chamador não o tenha feito. O módulo
continua fora do pacote/distribuição, portanto esse screen incompleto não
substitui o fallback softpipe no sistema gerado.

O HIDD recompilado tem 25.2 KiB. Os símbolos de criação/destruição do método
HIDD, `vc4_screen_create_aros` e o buffer manager estão definidos, sem
símbolos indefinidos no módulo.

## Recursos lineares iniciais

Foi criado o primeiro subconjunto de recursos Gallium em:

```text
workbench/hidds/vc4/vc4_resource_aros.c
workbench/hidds/vc4/vc4_resource_aros.h
```

Inicialmente ele aceitava somente `PIPE_BUFFER` linear. O suporte atual também
inclui texturas 2D lineares de nível zero. A implementação:

- copia o template Gallium e inicializa sua referência;
- cria um BO VC4 com o tamanho lógico do buffer;
- registra `can_create_resource`, `resource_create` e `resource_destroy` no
  `pipe_screen`;
- permite mapeamento de intervalos com validação de overflow/limites;
- libera o BO e a estrutura do recurso na destruição.

A fachada DRM ganhou `VC4_DRM_SYNC_BO`. Antes de uma leitura CPU,
`vc4_resource_map_aros()` executa `VC4_SYNC_GPU_TO_CPU`; depois de uma escrita,
`vc4_resource_unmap_aros()` executa `VC4_SYNC_CPU_TO_GPU`. Esses helpers são
usados pelos callbacks do contexto de transferência.

Sincronização de cache não é conversão de endianness. Dados genéricos do
buffer permanecem no formato definido pelo usuário/Mesa. Em um possível
build big-endian, command lists, shader records e outros pacotes lidos pelo
V3D precisarão ser emitidos explicitamente em little-endian; essa conversão
pertencerá ao encoder/submissão, não ao mapeamento genérico.

O primeiro link falhou porque o inline upstream de liberação de BO alcançava
o hash de handles compartilhados do Mesa. Como este backend não possui
contextos, jobs nem BOs compartilhados, a destruição usa diretamente a única
referência de propriedade do recurso. Isso deverá voltar ao reference
tracking completo quando jobs forem incorporados.

Após a correção, o HIDD compilou sem símbolos indefinidos e passou para
27.0 KiB. `vc4_resource_screen_init_aros`, `vc4_resource_map_aros` e
`vc4_resource_unmap_aros` estão definidos no módulo.

## Contexto de transferência

Foi adicionado:

```text
workbench/hidds/vc4/vc4_context_aros.c
workbench/hidds/vc4/vc4_context_aros.h
```

`context_create` agora devolve um `pipe_context` mínimo que implementa apenas:

```text
destroy
transfer_map
transfer_unmap
```

O mapa aceita `PIPE_BUFFER` e `PIPE_TEXTURE_2D` linear ou tiled. Buffers usam
somente nível zero; texturas usam qualquer nível válido do miptree.
Coordenadas negativas, intervalos fora do recurso,
`PIPE_TRANSFER_FLUSH_EXPLICIT` e recursos persistent/coherent são rejeitados.
A transferência mantém uma referência Gallium ao recurso até `unmap`, evitando
destruição do BO enquanto o ponteiro está em uso.

O fluxo efetivo é:

```text
pipe_context::transfer_map
    -> valida pipe_box e mantém referência
    -> vc4_resource_map_aros
    -> invalida cache se houver leitura
    -> devolve ponteiro AArch64 direto

pipe_context::transfer_unmap
    -> limpa cache se houve escrita
    -> solta referência do recurso
```

Não existem callbacks de draw, shader, estado, flush ou submissão. O screen
continua declarando `PIPE_CAP_ACCELERATED = 0` e permanece fora da
distribuição. O HIDD compilado tem 30.2 KiB e não possui símbolos indefinidos.

## Texturas 2D lineares

O backend aceita `PIPE_TEXTURE_2D` com:

- até 12 níveis;
- uma única camada e profundidade;
- sem multisampling;
- apenas `PIPE_BIND_LINEAR` ou bind zero;
- sem sampler view, render target, depth/stencil, scanout ou compartilhamento.

Stride, número de blocos e tamanho total usam os helpers oficiais
`util_format_*` do Mesa. O cálculo é feito em `size_t` e o recurso é rejeitado
se stride ou tamanho ultrapassarem `UINT32_MAX`, limite coerente com o
endereçamento do VC4. A biblioteca `libmesautil` passou a ser ligada ao HIDD
para fornecer a tabela real de descrições de formato.

As transferências 2D calculam offset, bytes por linha, número de linhas em
blocos e o intervalo completo de cache, incluindo gaps entre linhas. Para
formatos comprimidos, `x` e `y` devem estar alinhados ao bloco; largura e
altura podem terminar na borda parcial conforme o arredondamento do Mesa.

Neste marco, texturas são somente armazenamento linear acessível pela CPU:
não são anunciadas para sampling ou renderização. O HIDD resultante tem
aproximadamente 783.8 KiB, crescimento causado principalmente pela tabela de
formatos de `libmesautil`, e continua sem símbolos indefinidos.

## Autoteste opcional de BO e transferência

Foi adicionado:

```text
workbench/hidds/vc4/vc4_selftest.c
workbench/hidds/vc4/vc4_selftest.h
```

O teste é controlado em compilação por:

```c
#define VC4GALLIUM_SELFTEST 0
```

O padrão permanece desativado. Para um build destinado ao Raspberry Pi de
teste, a macro pode ser definida como `1`. O caminho habilitado foi compilado
com sucesso e depois o artefato final foi recompilado novamente com valor
zero.

Quando habilitado, após criar o screen o teste:

1. cria um `PIPE_BUFFER` de 16 KiB mais 257 bytes;
2. mapeia um intervalo não alinhado começando no byte 31;
3. escreve um padrão determinístico que atravessa muitas linhas de cache;
4. desmonta a transferência, executando `CPU_TO_GPU`;
5. remapeia para leitura, executando `GPU_TO_CPU`;
6. compara todos os bytes;
7. destrói contexto, recurso e BO em qualquer caminho de saída.

Em caso de divergência, o offset e os bytes são registrados em debug. O
`CreatePipeScreen` destrói o screen e retorna `NULL`, impedindo que um caminho
de memória incoerente prossiga silenciosamente.

Esse smoke test valida alocação por firmware, mapeamento, referências Gallium
e chamadas de manutenção de cache. Ele ainda não prova coerência após escrita
real da GPU; isso dependerá de submissão V3D.

## Tiling VC4 em CPU

As implementações upstream do Mesa 20.0.8 foram incorporadas através de:

```text
workbench/hidds/vc4/vc4_tiling_aros.c
workbench/hidds/vc4/vc4_tiling_lt_aros.c
```

Os wrappers compilam diretamente `vc4_tiling.c` e `vc4_tiling_lt.c` da árvore
Mesa preparada pelo build. O primeiro evita somente os includes
`vc4_screen.h`/`vc4_context.h`, que não são usados pelo algoritmo mas puxam
DRM/libdrm. Não há cópia divergente do algoritmo de endereçamento T/LT.

No AArch64, o helper comum `v3d_cpu_tiling.h` seleciona seu caminho SIMD
AArch64. O arquivo separado `vc4_tiling_lt_neon.c`, destinado ao caminho
AArch32/`USE_ARM_ASM`, não é compilado.

Estão definidos no HIDD:

```text
vc4_size_is_lt
vc4_load_tiled_image
vc4_store_tiled_image
vc4_load_lt_image_base
vc4_store_lt_image_base
```

O autoteste opcional ganhou dois round-trips adicionais:

- LT: imagem RGBA de 13 × 11, exercitando bordas não alinhadas a utile;
- T: imagem RGBA de 37 × 35, exercitando padding para tiles de 4 KiB.

Cada caso preenche a imagem linear, converte linear → tiled → linear e compara
todos os bytes lógicos. O caminho de teste habilitado compilou com sucesso;
a execução ainda depende do AROS em Raspberry Pi. O artefato final voltou a
ser compilado com o autoteste desativado.

O HIDD agora tem aproximadamente 788.9 KiB e continua sem símbolos
indefinidos.

## Recursos 2D tiled

O layout LT/T foi conectado ao backend de recursos:

- `PIPE_TEXTURE_2D` com bind zero usa tiling VC4;
- `PIPE_BIND_LINEAR` força o layout raster anterior;
- dimensões LT são arredondadas para utiles;
- dimensões T são arredondadas para blocos de 8 × 8 utiles;
- stride e tamanho padded são verificados antes de converter para 32 bits;
- o BO é criado com o tamanho físico padded, enquanto o template preserva as
  dimensões lógicas.

Formatos comprimidos/blocados continuam aceitos somente com
`PIPE_BIND_LINEAR`. O caminho tiled aceita cpp de 1, 2, 4 ou 8 bytes e formatos
com bloco 1 × 1, que correspondem às geometrias suportadas pelos helpers VC4.

Uma transferência tiled:

1. aloca staging linear do tamanho da região lógica;
2. invalida e mapeia o BO tiled inteiro se houver leitura;
3. executa `vc4_load_tiled_image()` para leitura;
4. entrega o staging ao caller;
5. em escrita, executa `vc4_store_tiled_image()`;
6. limpa o cache do BO tiled inteiro;
7. libera staging e a referência Gallium.

Sincronizar o BO inteiro é conservador, porém correto para o primeiro marco.
O refinamento por tiles pode ser feito após validação em hardware.

O autoteste opcional agora também cria uma textura RGBA 37 × 35 pelo
`pipe_screen`, escreve e relê usando `pipe_context::transfer_map/unmap`.
Assim, quando executado no Raspberry Pi, ele exercitará alocação do BO,
layout T, staging, conversões, cache e reference tracking no mesmo caminho
usado pelos callers Gallium. Os modos de teste ligado e desligado compilam;
o padrão final continua desligado.

Tiling reorganiza bytes e não troca sua ordem. A política big-endian continua
inalterada: pixels seguem o `pipe_format`; apenas estruturas/pacotes definidos
pelo hardware como little-endian devem receber conversão explícita.

## Miptrees iniciais

Texturas 2D agora aceitam até `VC4_MAX_MIP_LEVELS` (12 slices). O layout segue
a estratégia do Mesa VC4:

- os níveis são calculados do menor para o maior;
- níveis acima de zero usam dimensões derivadas da próxima potência de dois
  do nível base, como em `vc4_setup_slices()` upstream;
- cada nível escolhe LT ou T independentemente;
- offset, stride, tamanho e tiling são guardados em `slices[level]`;
- todos os slices são deslocados para alinhar o nível zero a 4 KiB;
- o BO cobre o miptree inteiro, incluindo padding;
- somas e alinhamentos usam 64 bits antes de validar `UINT32_MAX`.

As transferências validam o nível, usam suas dimensões lógicas e selecionam o
slice correspondente. Em textura tiled, somente o intervalo do slice
solicitado recebe manutenção de cache.

O autoteste Gallium tiled cria uma textura RGBA 37 × 35 com quatro níveis.
Cada nível recebe um padrão diferente e é relido antes do próximo, permitindo
detectar sobreposição de slices e erros no alinhamento do nível zero quando o
teste for executado. Os modos ligado e desligado compilam; o padrão final
continua desligado.

## Fronteira inicial de `SUBMIT_CL`

Foi reproduzido o layout de 176 bytes de `drm_vc4_submit_cl`, incluindo os
seis descritores `drm_vc4_submit_rcl_surface`. Há `_Static_assert` no recurso e
na fachada para detectar divergência de ABI.

O `vc4.resource` exporta agora:

```c
int VC4ValidateSubmitCL(const struct VC4SubmitCL *submit);
```

A validação atual:

- rejeita estrutura nula, padding não zero e `seqno` de entrada;
- rejeita perfmon e syncobj, ainda não implementados;
- aceita apenas os quatro flags VC4 conhecidos;
- exige dimensões não zero e limites de tiles ordenados;
- limita cada stream a 16 MiB;
- limita a tabela a 65.536 handles;
- valida multiplicações, overflow de ponteiro e alinhamento de quatro bytes;
- exige bin CL, shader records, uniforms e tabela de handles não vazios;
- resolve todos os handles sob o semáforo compartilhado de `vc4.resource`;
- valida cada `hindex` de superfície e seu offset contra o BO resolvido.

Antes da resolução, o recurso agora aloca snapshots privados de bin CL,
shader records, uniforms e handles, limitando a soma a 32 MiB. Os quatro
blocos são copiados uma vez; toda validação posterior usa somente essas
cópias e elas são liberadas antes do retorno. Nenhum ponteiro do caller é
retido.

A fachada implementa `VC4_DRM_SUBMIT_CL`. Ela copia a estrutura de transporte
para a ABI nativa, chama `VC4ValidateSubmitCL` e, se tudo estiver válido,
retorna:

```text
VC4_SUBMIT_ERR_NOT_IMPLEMENTED
```

Portanto, nenhuma escrita em registradores V3D ocorre. Validação e execução
estão deliberadamente separadas.

Como AROS usa um único address space, checar aritmética/alinhamento não prova
que um endereço arbitrário esteja legível: um ponteiro completamente inválido
ainda pode causar fault durante `CopyMem`. Depois da cópia, porém, mutações do
caller não afetam a resolução de handles nem a futura validação profunda.

O autoteste opt-in monta uma submissão mínima com um BO real. A estrutura
válida deve alcançar `NOT_IMPLEMENTED`; ao substituir o handle por
`UINT32_MAX`, deve receber `INVALID`. Esse caminho habilitado compilou.

O snapshot da bin CL agora é decodificado pacote a pacote antes de aceitar a
submissão. O decoder:

- aceita somente o subconjunto usado pelo validador VC4 do Mesa 20.0.8;
- valida o tamanho fixo de cada pacote sem avançar além do snapshot;
- rejeita opcode desconhecido, `HALT` e pacotes de renderização no BCL;
- exige uma única configuração de binning antes de `START_TILE_BINNING`;
- rejeita dimensões de tile nulas e os modos unsupported DB/non-MS e 64-bit;
- exige `INCREMENT_SEMAPHORE` e `FLUSH` nas duas últimas posições;
- decodifica os campos de 32 bits explicitamente como little-endian;
- valida os dois índices do pseudo-pacote `GEM_HANDLES`;
- exige shader state antes de primitivas e um BO selecionado antes de
  primitivas indexadas;
- limita referências de shader record à quantidade declarada.

Essa etapa identifica as referências que precisarão de relocation, mas ainda
não altera a command list nem calcula endereços de barramento. O autoteste foi
atualizado para enviar a sequência mínima válida de quatro pacotes.

Os `GL_SHADER_STATE` decodificados agora alimentam uma segunda passagem sobre
o snapshot de shader records. Para cada estado ela:

- interpreta os formatos normal e extended usados pelo VC4;
- calcula a quantidade de atributos, o tamanho do record e sua tabela de
  relocations;
- valida os hindices dos três shaders e de todos os vertex buffers;
- exige offset zero para FS, VS e CS, como o ABI do Mesa 20.0.8;
- acompanha o maior índice usado pelas primitivas;
- prova que `offset + tamanho + índice * stride` cabe no BO de atributo;
- aceita no máximo 15 bytes finais de padding, somente se forem zero;
- limita os metadados auxiliares a 65.536 shader states.

`VC4CreateBO` ganhou a marca `VC4_BOF_SHADER`. A fachada usa essa marca em
`CREATE_SHADER_BO`, e a validação impede misturar shader BOs e BOs de dados
nos shader records. Flags desconhecidas de criação de BO agora falham.

Ainda falta validar completamente as instruções QPU dentro dos três shader
BOs. Portanto, um BO estar marcado como shader e passar pelas verificações
estruturais não o torna seguro para execução; `SUBMIT_CL` continua terminando
em `NOT_IMPLEMENTED`.

Uma primeira passagem conservadora de instruções QPU foi adicionada. O BO
agora preserva separadamente o tamanho lógico solicitado e o tamanho físico
arredondado para página; apenas o tamanho lógico entra no decoder. A passagem:

- exige código não vazio, múltiplo de 64 bits e com espaço para `PROG_END`
  mais seus dois delay slots;
- lê cada instrução explicitamente como little-endian;
- aceita apenas os sinais QPU reconhecidos pelo validador upstream;
- rejeita breakpoint e sinais de load não suportados;
- rejeita imediatamente writes para host interrupt, TMU no-swap, alpha mask,
  VPM DMA address e mutex release;
- aceita somente branches relativos, sem endereço vindo de registrador;
- exige branch alinhado, com os dois caminhos dentro do shader BO;
- exige que branch não escreva registradores;
- exige terminação explícita por `PROG_END`.

Essa passagem reduz a superfície aceita, mas ainda não prova segurança
completa. Falta portar a análise de data-flow do upstream para uniforms,
configuração TMU, clamps, resets de uniform address e VPM.
Por isso ela não muda a decisão de manter execução desabilitada.

O decoder QPU agora também conta as leituras diretas do FIFO de uniforms,
respeitando a diferença entre `SMALL_IMM`, `LOAD_IMM` e instruções ALU. A soma
mínima dos três shaders de cada shader record não pode ultrapassar o snapshot
de uniforms, cujo tamanho deve ser múltiplo de 32 bits. Isso detecta overreads
simples; parâmetros implícitos de TMU e resets de uniform address ainda não
entram na conta e continuam pendentes.

As invariantes independentes de data-flow para shaders threaded também foram
portadas: switches precisam estar separados pelos delay slots, branches não
podem ocorrer dentro deles e um shader threaded não pode usar a metade
superior dos register files.

O estado das duas TMUs agora é acompanhado separadamente. Cada sequência pode
ter no máximo quatro parâmetros antes do write de submissão em `S`; uma
sequência incompleta não pode atravessar branch, thread switch ou `PROG_END`.
Os parâmetros implícitos e o hindex de textura são incluídos no mínimo exigido
do stream de uniforms. Writes simultâneos pelas pipelines ADD e MUL e leituras
de uniform no mesmo ciclo da configuração TMU são rejeitados.

O modo TMU direto e shaders que combinam TMU com branches permanecem
deliberadamente rejeitados. Suportá-los com segurança requer a análise
upstream de clamps e estados por basic block, que ainda não foi portada.

O resultado da análise QPU agora separa `texture_count` de
`uniform_data_bytes`. A passagem dos shader records usa esses valores para
caminhar pelo snapshot de uniforms na mesma organização do Mesa:

```text
[hindices das texturas][dados consumidos pelo shader]
```

Cada hindex é decodificado como little-endian, validado contra a tabela da
submissão e resolvido sob o lock de `vc4.resource`. BOs ausentes ou marcados
como shader não podem ser usados como textura. Ao final, só são tolerados até
15 bytes de padding zero; dados extras não interpretados são rejeitados.

Uma segunda passagem QPU agora reconstrói os offsets P0-P3 de cada amostra na
ordem exata em que os writes TMU consomem uniforms. Ela exige pelo menos P0 e
P1, associa a amostra ao hindex correspondente e valida que o offset-base de
P0, alinhado a 4 KiB pelo formato VC4, esteja dentro do BO resolvido. A
passagem também comprova que a quantidade de amostras e todos os bytes
consumidos coincidem com o resultado da primeira análise QPU.

Essa reconstrução fornece a entrada necessária para portar progressivamente
o cálculo de `reloc_tex()` upstream. Nenhuma relocation é escrita.

P0/P1 agora são decodificados para a textura de nível base. O validador:

- extrai largura e altura de 11 bits, incluindo o valor especial zero = 2048;
- combina os quatro bits de tipo de P0 com o quinto bit de P1;
- aceita os formatos de 1, 2 e 4 bytes suportados pelo upstream, além de ETC1;
- converte ETC1 para blocos de 4 × 4 com 8 bytes;
- reproduz a seleção automática linear, LT ou T do hardware;
- alinha a área a utile, ou a macrotiles 8 × 8 no modo T;
- usa aritmética de 64 bits e exige `offset + level_size <= bo_Size`.

Mip levels agora são caminhados em ordem reversa a partir do endereço-base,
como no validador upstream. Cada nível reduz dimensões até 1 × 1, recalcula o
alinhamento e pode fazer a transição T→LT. Toda subtração é verificada para
impedir underflow antes de alcançar memória anterior ao BO.

Cube maps também são aceitos quando exatamente um P2 ou P3 fornece um stride
não zero. O cálculo em 64 bits valida `base + 5 * stride + tamanho`, cobrindo
a sexta face. Stride ausente ou duplicado é rejeitado.

Formatos que o validador upstream considera inseguros permanecem rejeitados.
Nenhuma relocation é escrita.

`vc4.resource` passou para aproximadamente 20.6 KiB e define o vetor
`Vc4_6_VC4ValidateSubmitCL`. O HIDD tem aproximadamente 789.8 KiB. Ambos
compilam sem símbolos indefinidos.

## Próximo incremento

Substituir progressivamente o screen de sondagem pelo `vc4_screen.c` real. O
próximo subconjunto deve aprofundar a validação sem executar:

- completar o validador QPU com clamps, basic blocks, modo TMU direto e VPM;
- validar P0-P3 e os limites físicos de cada texture BO;
- validar child images codificadas pelos outros tipos de P2/P3;
- validar uniforms e referências de textura;
- produzir uma command list privada relocada;
- manter execução desativada até existir uma lista validada e relocada.

## Referências locais

- `arch/arm-native/soc/broadcom/2708/hidd/vc4gfx/`
- `arch/aarch64-raspi/boot/mmakefile.src`
- `workbench/hidds/gallium/`
- `workbench/hidds/softpipe/`
- `workbench/libs/mesa/mesa.cfg`
- `workbench/libs/mesa/mesa-20.0.8-aros.diff`
- `workbench/libs/mesa/mesa3dgl_gallium.c`
