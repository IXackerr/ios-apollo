#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:

  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]                  # sweet
  mkadt.py OUT.adt --board apollo --phys-base HEX --mem-size HEX  # Mi 10T (Pro)
           [--touch-fw novatek_nt36675_j3s_fw01.bin] [--gpu-fw DIR]
           [--run PROGRAM [--run-args "ARGS"] [--run-delay SECONDS] [--run-file NAME=FILE]...]
           [--metal-file NAME=FILE]... [--display-gamut sRGB|P3]

--gpu-fw DIR adds /sweet-gpu with the phone's own a650_sqe.fw, a650_gmu.bin
and a650_zap (.mbn, or .mdt + .bNN which are joined like tools/mkzap.py)
for SweetAdreno (patch 0094). The blobs are Qualcomm/Xiaomi proprietary:
copy them from the phone's /vendor/firmware and keep them outside git; the
ADT is a local build product.

--run PROGRAM adds /sweet-run: ios/sweetrun, started by launchd at boot,
runs PROGRAM (an iOS Mach-O) with ARGS after SECONDS and its output goes
to the console. Tests reach the phone in the boot image, the root stays
as it is. --run-file NAME=FILE (repeatable; --run-lib FILE is lib=FILE)
adds more files for the program, zlib-compressed (properties NAME-z,
NAME-0, NAME-1, ...; ios/sweetrun/sweetrun.h sweetrun_file), e.g. the
Vulkan driver.

--metal-file NAME=FILE (repeatable) adds /sweet-metal: the Metal driver
the SweetMetal loader on the root unpacks (gpu/userland/sweetmetal/loader),
the same encoding plus an "id" that changes with the files.

--display-gamut overrides /product artwork-display-gamut (D22AP: P3).

For apollo the RAM window has no default on purpose: take it from the
phone's live FDT with tools/fdtmem.py (see PORTING-APOLLO.md). The touch
firmware is built into the phone's stock kernel; without --touch-fw the
/sweet-touch node still names the controller, and the driver stays idle.
"""
import hashlib, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# Per-board differences. 'xiaomi,sweet' stays in every root compatible:
# SweetPlatformExpert and the SWEET drivers match on it.
# apollo values: mainline sm8250.dtsi + sm8250-xiaomi-apollo.dts (PORTING-APOLLO.md).
BOARDS = {
    'sweet': dict(
        soc_compat=['xiaomi,sweet', 'qcom,sm7150'],
        vtimer_intid=VTIMER_INTID,
        cpu_compat='qcom,kryo470',
        # cores 0-5 Cortex-A55, 6-7 Cortex-A76
        cpu_hz=[1804800000] * 6 + [2304000000] * 2,
        memory_hz=1866000000,
        keys=None,  # kernel defaults: PM6150L GPIO2 (SID 4) for volume up
        touch=None,  # kernel probes GT9896 / FT on QUP1 SE7
        cpufreq=None,  # left as the bootloader set it
    ),
    'apollo': dict(
        soc_compat=['xiaomi,sweet', 'xiaomi,apollo', 'qcom,sm8250'],
        # sm8250.dtsi timer: virtual timer <GIC_PPI 11> = INTID 27 (sweet: 19).
        # The shim's no-payload probe confirms it on the phone.
        vtimer_intid=27,
        cpu_compat='qcom,kryo585',
        # cores 0-3 Cortex-A55, 4-6 Cortex-A77, 7 Cortex-A77 prime
        cpu_hz=[1804800000] * 4 + [2419200000] * 3 + [2841600000],
        memory_hz=2750000000,
        # Volume up: PM8150 GPIO6 (SID 0, peripheral 0xc5, gpio-keys in the
        # apollo DTS). Power / volume down: PM8150 PON (PPID 0x008), as on sweet.
        keys=dict(volup_ppid=0x0c5, volup_base=0xc500),
        # NT36675 on QUP0 SE4 (bsd/dev/sweet_nt36675.c, patch 0086)
        touch='novatek,nt36675',
        # EPSS frequency domains (A55 / A77 / A77 prime) and the targets
        # bsd/dev/sweet_cpufreq.c sets (patch 0088). ABL leaves the A77s at
        # their lowest level; without the vendor thermal (LMh) setup the big
        # cores stay well below their 2419 / 2841 MHz maximum.
        cpufreq=[(0x18591000, 1804), (0x18592000, 2000), (0x18593000, 2000)],
    ),
}

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


# Large files in /sweet-run are split: one 17 MB property never reached
# user space through IORegistryEntryCreateCFProperty.
RUN_CHUNK = 512 * 1024


def file_props(files):
    """Files as zlib streams in properties NAME-0, NAME-1, ..., the size in NAME-z."""
    props = []
    for name, data in files:
        z = zlib.compress(data, 9)
        props.append((f'{name}-z', u32(len(data))))
        props += [(f'{name}-{i}', z[o:o + RUN_CHUNK])
                  for i, o in enumerate(range(0, len(z), RUN_CHUNK))]
    return props


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def gpu_node(fwdir, ph):
    """/sweet-gpu for bsd/dev/sweet_gpu_core.c (load_firmware_refs)."""
    def blob(*names):
        for n in names:
            p = os.path.join(fwdir, n)
            if os.path.exists(p):
                return open(p, 'rb').read()
        return None
    sqe = blob('a650_sqe.fw')
    gmu = blob('a650_gmu.bin')
    if sqe is None or gmu is None:
        raise SystemExit(f'--gpu-fw {fwdir}: need a650_sqe.fw and a650_gmu.bin')
    zap = blob('a650_zap.mbn', 'a650_zap.elf')
    if zap is None and os.path.exists(os.path.join(fwdir, 'a650_zap.mdt')):
        import mkzap  # tools/mkzap.py
        zap = mkzap.join(os.path.join(fwdir, 'a650_zap.mdt'))
    props = [
        ph(),
        ('compatible', cstr('qcom,adreno-650.2')),
        ('chip-id', u32(0x06050002)),
        ('sqe-firmware', sqe),
        ('gmu-firmware', gmu),
        # pil_gpu_region from the stock DT (sm8250 gpu_mem): TZ authenticates
        # the zap image there (PAS id 13)
        ('zap-region', u64(0x8681a000, 0x2000)),
    ]
    if zap is not None:
        props.append(('zap-firmware', zap))
    else:
        print('mkadt: no a650_zap in --gpu-fw: secure mode can only be left on an '
              'unlocked TZ (RBBM_SECVID_TRUST_CNTL)', file=sys.stderr)
    print(f'mkadt: /sweet-gpu sqe {len(sqe)} gmu {len(gmu)} zap {len(zap) if zap else 0} bytes',
          file=sys.stderr)
    return node('sweet-gpu', props)


def build(seed=None, qemu=False, highmem=False, board='sweet', phys_base=None, mem_size=None,
          touch_fw=None, gpu_fw=None, run=None, metal=None, display_gamut=None):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    b = BOARDS[board]
    if phys_base is None or mem_size is None:
        if board != 'sweet' and not qemu:
            raise SystemExit(f'--board {board}: give --phys-base and --mem-size '
                             '(tools/fdtmem.py LIVE.dtb prints them)')
        phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    vtimer_intid = QEMU_VTIMER_INTID if qemu else b['vtimer_intid']
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(b['cpu_compat']) + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(b['cpu_hz'][core])),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(b['memory_hz'])),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer_intid)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])
    # PMIC arbiter (same on sweet and apollo): the platform expert reads the
    # PMIC RTC through it only when this node exists. Absent under QEMU.
    spmi = node('spmi', [
        ph(),
        ('compatible', cstr('qcom,spmi-pmic-arb')),
        ('reg', u64(0x0c440000, 0x1100)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    # Board key wiring for bsd/dev/sweet_keys.c (patch 0082); absent = sweet.
    extra = []
    if b['keys']:
        extra.append(node('sweet-keys', [
            ph(),
            ('volup-ppid', u32(b['keys']['volup_ppid'])),
            ('volup-base', u32(b['keys']['volup_base'])),
        ]))
    if b['cpufreq'] and not qemu:
        extra.append(node('sweet-cpufreq', [
            ph(),
            ('compatible', cstr('qcom,cpufreq-epss')),
            ('reg', b''.join(u64(base, 0x1000) for base, _ in b['cpufreq'])),
            ('target-mhz', b''.join(u32(mhz) for _, mhz in b['cpufreq'])),
        ]))
    if gpu_fw:
        if board != 'apollo':
            raise SystemExit('--gpu-fw: only the apollo (SM8250 / Adreno 650) board')
        extra.append(gpu_node(gpu_fw, ph))
    if run:
        # read by ios/sweetrun at boot: a program to run without changing the root
        program, args, delay, files = run
        props = [
            ph(),
            ('program', program),
            ('args', b''.join(cstr(a) for a in args)),
            ('delay', u32(delay)),
        ]
        extra.append(node('sweet-run', props + file_props(files)))
    if metal:
        # read by the SweetMetal loader on the root: the Metal driver itself,
        # unpacked once per boot image into /private/var/sweetmetal/<id>
        digest = hashlib.sha256(b''.join(n.encode() + d for n, d in metal)).hexdigest()[:16]
        extra.append(node('sweet-metal', [ph(), ('id', cstr(digest))] + file_props(metal)))
    if b['touch']:
        extra.append(node('sweet-touch', [
            ph(),
            ('compatible', cstr(b['touch'])),
        ] + ([('firmware', touch_fw)] if touch_fw else [])))

    product_props = d22ap_product()
    if product_props and display_gamut:
        # MobileGestalt's HasExtendedColorDisplay is ArtworkDisplayGamut == P3,
        # and with it CoreAnimation renders into two-plane 10-bit (b3a8)
        # framebuffers; "sRGB" keeps them BGRA8, which the display shows.
        product_props = [(k, cstr(display_gamut) if k == 'artwork-display-gamut' else v)
                         for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + b''.join(cstr(c) for c in b['soc_compat']) + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + extra + product + ([] if qemu else [ufs, spmi]))


def run_files(option='--run-file'):
    """OPTION NAME=FILE (repeatable); --run-lib FILE is --run-file lib=FILE."""
    files = []
    for i, a in enumerate(sys.argv):
        if a in (option, '--run-lib' if option == '--run-file' else None) and i + 1 < len(sys.argv):
            spec = sys.argv[i + 1] if a == option else 'lib=' + sys.argv[i + 1]
            name, _, path = spec.partition('=')
            files.append((name, open(path, 'rb').read()))
    return files


def _opt(name, conv=str, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 >= len(sys.argv):
            raise SystemExit(f'{name} needs a value')
        return conv(sys.argv[i + 1])
    return default


if __name__ == '__main__':
    board = _opt('--board', default='sweet')
    if board not in BOARDS:
        raise SystemExit(f'unknown --board {board} ({", ".join(BOARDS)})')
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv, board=board,
                 phys_base=_opt('--phys-base', lambda x: int(x, 0)),
                 mem_size=_opt('--mem-size', lambda x: int(x, 0)),
                 touch_fw=_opt('--touch-fw', lambda f: open(f, 'rb').read()),
                 gpu_fw=_opt('--gpu-fw'),
                 run=_opt('--run', lambda f: (open(f, 'rb').read(),
                                              _opt('--run-args', default='').split(),
                                              _opt('--run-delay', int, 0),
                                              run_files())),
                 metal=run_files('--metal-file'),
                 display_gamut=_opt('--display-gamut'))
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
