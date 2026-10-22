//! sweetmetal-core: the C ABI (`include/sweetmetal.h`) that SweetMetal.bundle's Objective-C
//! classes call, over metal2vulkan-runtime (the Metal object model on Vulkan) and metal2vulkan
//! (AIR -> SPIR-V). Below it: Vulkan = Mesa turnip with the `sweet` kernel backend, then the
//! SweetAdreno XNU driver.
//!
//! Every handle is a `Box` of the runtime's own (clonable, `Send + Sync`) handle; encoders are
//! owned values that `*_end` consumes. Errors are kept per thread for `smtl_last_error`.
#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::PathBuf;
use std::time::Duration;

use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues};
use metal2vulkan::metal::state::{DepthStencilDescriptor, StencilDescriptor};
use metal2vulkan::metal::{CompareFunction, PixelFormat, PrimitiveType, StencilOperation, TextureUsage};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor,
    VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan_runtime::{
    BlitCommandEncoder, BlitOption, Buffer, ClearColor, CommandBuffer, CommandBufferStatus,
    CommandQueue, ComputeCommandEncoder, ComputePipelineState, DepthStencilState, Device,
    DeviceOptions, Function,
    HostMemory, IoSurfacePlane, Library, Region, LoadAction, Origin, PipelineOptions, RenderCommandEncoder,
    RenderPassDescriptor, RenderPipelineState, ResourceOptions, Size, StoreAction, Texture,
    TextureDescriptor,
};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_error(msg: String) {
    let msg = CString::new(msg.replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = msg);
}

/// `Result` to pointer: `Box` on success, NULL + last error on failure.
fn boxed<T, E: std::fmt::Debug>(what: &str, r: Result<T, E>) -> *mut T {
    match r {
        Ok(v) => Box::into_raw(Box::new(v)),
        Err(e) => {
            set_error(format!("{what}: {e:?}"));
            std::ptr::null_mut()
        }
    }
}

/// `Result` to status: 0 on success, -1 + last error on failure.
fn status<E: std::fmt::Debug>(what: &str, r: Result<(), E>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(e) => {
            set_error(format!("{what}: {e:?}"));
            -1
        }
    }
}

macro_rules! handle {
    ($p:expr, $ret:expr) => {
        match unsafe { $p.as_mut() } {
            Some(h) => h,
            None => {
                set_error(format!("{}: NULL handle", stringify!($p)));
                return $ret;
            }
        }
    };
}

macro_rules! release {
    ($name:ident, $t:ty) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(p: *mut $t) {
            if !p.is_null() {
                drop(Box::from_raw(p));
            }
        }
    };
}

fn pixel_format(raw: u32) -> Result<PixelFormat, String> {
    PixelFormat::try_from(raw).map_err(|e| format!("MTLPixelFormat {raw}: {e:?}"))
}

#[no_mangle]
pub extern "C" fn smtl_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

// ---------------------------------------------------------------- device

/// The device plus a NUL-terminated copy of its name for `smtl_device_name`.
pub struct SmtlDevice {
    device: Device,
    name: CString,
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_create(
    vulkan_library: *const c_char,
    shader_cache_dir: *const c_char,
) -> *mut SmtlDevice {
    let mut options = DeviceOptions::default();
    if !vulkan_library.is_null() {
        options.loader = Some(PathBuf::from(
            CStr::from_ptr(vulkan_library).to_string_lossy().into_owned(),
        ));
    }
    if !shader_cache_dir.is_null() {
        options.shader_cache.directory = Some(PathBuf::from(
            CStr::from_ptr(shader_cache_dir).to_string_lossy().into_owned(),
        ));
    }
    boxed(
        "smtl_device_create",
        Device::system_default_with(&options).map(|device| {
            let name = CString::new(device.name()).unwrap_or_default();
            SmtlDevice { device, name }
        }),
    )
}
release!(smtl_device_release, SmtlDevice);

#[no_mangle]
pub unsafe extern "C" fn smtl_device_name(d: *const SmtlDevice) -> *const c_char {
    d.as_ref().map_or(std::ptr::null(), |d| d.name.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_registry_id(d: *const SmtlDevice) -> u64 {
    d.as_ref().map_or(0, |d| d.device.registry_id())
}

// ---------------------------------------------------------------- buffers

fn resource_options(raw: u64) -> Result<ResourceOptions, String> {
    ResourceOptions::from_raw(raw).map_err(|e| format!("MTLResourceOptions {raw:#x}: {e:?}"))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_buffer(
    d: *mut SmtlDevice,
    length: u64,
    options: u64,
) -> *mut Buffer {
    let d = handle!(d, std::ptr::null_mut());
    boxed(
        "newBufferWithLength:options:",
        resource_options(options)
            .and_then(|o| d.device.new_buffer(length, o).map_err(|e| format!("{e:?}"))),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_buffer_with_bytes(
    d: *mut SmtlDevice,
    bytes: *const c_void,
    length: u64,
    options: u64,
) -> *mut Buffer {
    let d = handle!(d, std::ptr::null_mut());
    if bytes.is_null() {
        set_error("newBufferWithBytes: NULL bytes".into());
        return std::ptr::null_mut();
    }
    let data = std::slice::from_raw_parts(bytes as *const u8, length as usize);
    boxed(
        "newBufferWithBytes:length:options:",
        resource_options(options).and_then(|o| {
            d.device
                .new_buffer_with_bytes(data, o)
                .map_err(|e| format!("{e:?}"))
        }),
    )
}

/// `newBufferWithBytesNoCopy:length:options:deallocator:` without a deallocator: the GPU uses
/// the caller's pages at `bytes` (VK_EXT_external_memory_host); they must stay mapped while the
/// buffer lives. Also how an IOSurface's memory becomes a buffer.
#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_buffer_no_copy(
    d: *mut SmtlDevice,
    bytes: *mut c_void,
    length: u64,
    options: u64,
) -> *mut Buffer {
    let d = handle!(d, std::ptr::null_mut());
    let Some(pointer) = std::ptr::NonNull::new(bytes as *mut u8) else {
        set_error("newBufferWithBytesNoCopy: NULL bytes".into());
        return std::ptr::null_mut();
    };
    boxed(
        "newBufferWithBytesNoCopy:length:options:deallocator:",
        resource_options(options).and_then(|o| {
            d.device
                .new_buffer_with_bytes_no_copy(HostMemory::new(pointer), length, o, None)
                .map_err(|e| format!("{e:?}"))
        }),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_contents(b: *const Buffer) -> *mut c_void {
    b.as_ref()
        .and_then(|b| b.contents())
        .map_or(std::ptr::null_mut(), |p| p.as_ptr() as *mut c_void)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_length(b: *const Buffer) -> u64 {
    b.as_ref().map_or(0, |b| b.length())
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_gpu_address(b: *const Buffer) -> u64 {
    b.as_ref().map_or(0, |b| b.gpu_address().0)
}
release!(smtl_buffer_release, Buffer);

// ---------------------------------------------------------------- textures

fn texture_descriptor(
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    mipmapped: bool,
    usage: u32,
    options: u64,
) -> Result<TextureDescriptor, String> {
    let mut desc =
        TextureDescriptor::texture_2d(pixel_format(pixel_format_raw)?, width, height, mipmapped);
    desc.usage = TextureUsage::try_from(usage).map_err(|e| format!("MTLTextureUsage: {e:?}"))?;
    desc.resource_options = resource_options(options)?;
    Ok(desc)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_texture_2d(
    d: *mut SmtlDevice,
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    mipmapped: i32,
    usage: u32,
    resource_options_raw: u64,
) -> *mut Texture {
    let d = handle!(d, std::ptr::null_mut());
    boxed(
        "newTextureWithDescriptor:",
        texture_descriptor(pixel_format_raw, width, height, mipmapped != 0, usage, resource_options_raw)
            .and_then(|desc| d.device.new_texture(&desc).map_err(|e| format!("{e:?}"))),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_buffer_new_iosurface_texture(
    surface: *mut Buffer,
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    usage: u32,
    offset: u64,
    bytes_per_row: u64,
    planar: i32,
) -> *mut Texture {
    let surface = handle!(surface, std::ptr::null_mut());
    boxed(
        "newTextureWithDescriptor:iosurface:plane:",
        texture_descriptor(pixel_format_raw, width, height, false, usage, 0).and_then(|desc| {
            surface
                .new_texture_with_iosurface_plane(
                    &desc,
                    IoSurfacePlane::new(offset, bytes_per_row, planar != 0),
                )
                .map_err(|e| format!("{e:?}"))
        }),
    )
}
release!(smtl_texture_release, Texture);

/// `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:`; `len` is how many
/// bytes at `bytes` may be read.
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_replace_region(
    t: *mut Texture,
    region: *const u64,             // x, y, z, width, height, depth
    level: u64,
    slice: u64,
    bytes: *const c_void,
    len: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
) -> i32 {
    let t = handle!(t, -1);
    if region.is_null() || bytes.is_null() {
        set_error("replaceRegion: NULL region or bytes".into());
        return -1;
    }
    let r = std::slice::from_raw_parts(region, 6);
    let data = std::slice::from_raw_parts(bytes as *const u8, len as usize);
    status(
        "replaceRegion:",
        t.replace_region(
            Region::new_3d(r[0], r[1], r[2], r[3], r[4], r[5]),
            level,
            slice,
            data,
            bytes_per_row,
            bytes_per_image,
        ),
    )
}

/// `getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:`
#[no_mangle]
pub unsafe extern "C" fn smtl_texture_get_bytes(
    t: *mut Texture,
    bytes: *mut c_void,
    len: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
    region: *const u64,
    level: u64,
    slice: u64,
) -> i32 {
    let t = handle!(t, -1);
    if region.is_null() || bytes.is_null() {
        set_error("getBytes: NULL region or bytes".into());
        return -1;
    }
    let r = std::slice::from_raw_parts(region, 6);
    let data = std::slice::from_raw_parts_mut(bytes as *mut u8, len as usize);
    status(
        "getBytes:",
        t.get_bytes(
            data,
            bytes_per_row,
            bytes_per_image,
            Region::new_3d(r[0], r[1], r[2], r[3], r[4], r[5]),
            level,
            slice,
        ),
    )
}

// ---------------------------------------------------------------- shaders, pipelines

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_library_with_data(
    d: *mut SmtlDevice,
    metallib: *const c_void,
    len: usize,
) -> *mut Library {
    let d = handle!(d, std::ptr::null_mut());
    let data = std::slice::from_raw_parts(metallib as *const u8, len);
    boxed("newLibraryWithData:error:", d.device.new_library_with_data(data))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_library_with_air(
    d: *mut SmtlDevice,
    bitcode: *const c_void,
    len: usize,
) -> *mut Library {
    let d = handle!(d, std::ptr::null_mut());
    let data = std::slice::from_raw_parts(bitcode as *const u8, len);
    boxed(
        "library from AIR bitcode",
        metal2vulkan::library::Library::from_air(data)
            .map(|lib| d.device.new_library_with_compile_library(lib)),
    )
}
release!(smtl_library_release, Library);

#[no_mangle]
pub unsafe extern "C" fn smtl_library_new_function(
    l: *mut Library,
    name: *const c_char,
) -> *mut Function {
    let l = handle!(l, std::ptr::null_mut());
    if name.is_null() {
        set_error("newFunctionWithName: NULL name".into());
        return std::ptr::null_mut();
    }
    let name = CStr::from_ptr(name).to_string_lossy();
    boxed("newFunctionWithName:", l.new_function_with_name(&name))
}
release!(smtl_function_release, Function);

/// Packed byte size of a function constant of MTLDataType `raw` (scalars and vectors), 0 for
/// another type.
fn constant_size(raw: u32) -> usize {
    let (first, scalar) = match raw {
        3..=6 => (3, 4),        // float
        16..=19 => (16, 2),     // half
        29..=32 => (29, 4),     // int
        33..=36 => (33, 4),     // uint
        37..=40 => (37, 2),     // short
        41..=44 => (41, 2),     // ushort
        45..=48 => (45, 1),     // char
        49..=52 => (49, 1),     // uchar
        53..=56 => (53, 1),     // bool
        81..=84 => (81, 8),     // long
        85..=88 => (85, 8),     // ulong
        121..=124 => (121, 2),  // bfloat
        _ => return 0,
    };
    (raw - first + 1) as usize * scalar
}

/// Asked for the value of each constant a function declares: write `len` bytes to `out` and
/// answer 1, or answer 0 for a constant without a value.
pub type SmtlConstantLookup = unsafe extern "C" fn(
    ctx: *mut c_void,
    name: *const c_char,
    index: u32,
    data_type: u32,
    out: *mut c_void,
    len: u32,
) -> i32;

/// `newFunctionWithName:constantValues:error:` (and the descriptor form): the function `name`,
/// specialized with the values `lookup` gives for the constants it declares. Metal's
/// MTLFunctionConstantValues has no public getter, so the caller answers per constant.
#[no_mangle]
pub unsafe extern "C" fn smtl_library_new_function_with_constants(
    l: *mut Library,
    name: *const c_char,
    lookup: SmtlConstantLookup,
    ctx: *mut c_void,
) -> *mut Function {
    let l = handle!(l, std::ptr::null_mut());
    if name.is_null() {
        set_error("newFunctionWithName:constantValues: NULL name".into());
        return std::ptr::null_mut();
    }
    let name = CStr::from_ptr(name).to_string_lossy();
    let declared = match l
        .new_function_with_name(&name)
        .and_then(|f| f.function_constants_dictionary())
    {
        Ok(d) => d,
        Err(e) => {
            set_error(format!("newFunctionWithName:constantValues: {e:?}"));
            return std::ptr::null_mut();
        }
    };
    let mut values = FunctionConstantValues::defaults();
    for c in declared.values() {
        let ty = c.data_type();
        let size = constant_size(ty.as_raw());
        if size == 0 {
            continue;
        }
        let mut bytes = vec![0u8; size];
        let cname = CString::new(c.name()).unwrap_or_default();
        if lookup(ctx, cname.as_ptr(), c.index(), ty.as_raw(), bytes.as_mut_ptr().cast(), size as u32) == 1 {
            values = values.with_index(c.index(), FunctionConstantValue::new(ty, bytes));
        }
    }
    boxed(
        "newFunctionWithName:constantValues:error:",
        l.new_function_with_name_constant_values(&name, &values),
    )
}

/// `functionType`: the MTLFunctionType raw value, 0 when it cannot be told.
#[no_mangle]
pub unsafe extern "C" fn smtl_function_type(f: *const Function) -> u32 {
    f.as_ref()
        .and_then(|f| f.function_type().ok())
        .map_or(0, |t| t.as_raw())
}

/// `functionNames`: the library's function names, NUL-separated into `out` (`out_len` bytes,
/// the list cut short where it does not fit); the bytes the whole list needs.
#[no_mangle]
pub unsafe extern "C" fn smtl_library_function_names(
    l: *const Library,
    out: *mut c_char,
    out_len: usize,
) -> usize {
    let Some(l) = l.as_ref() else { return 0 };
    let mut list = Vec::new();
    for name in l.function_names() {
        list.extend_from_slice(name.as_bytes());
        list.push(0);
    }
    if !out.is_null() {
        let n = list.len().min(out_len);
        std::ptr::copy_nonoverlapping(list.as_ptr(), out as *mut u8, n);
    }
    list.len()
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_compute_pipeline(
    d: *mut SmtlDevice,
    f: *mut Function,
) -> *mut ComputePipelineState {
    let d = handle!(d, std::ptr::null_mut());
    let f = handle!(f, std::ptr::null_mut());
    boxed(
        "newComputePipelineStateWithFunction:error:",
        d.device
            .new_compute_pipeline_state_with_function(f, &PipelineOptions::default()),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_max_threads(p: *const ComputePipelineState) -> u64 {
    p.as_ref()
        .map_or(0, |p| p.max_total_threads_per_threadgroup() as u64)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_pipeline_execution_width(
    p: *const ComputePipelineState,
) -> u64 {
    p.as_ref().map_or(0, |p| p.thread_execution_width() as u64)
}
release!(smtl_compute_pipeline_release, ComputePipelineState);

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_render_pipeline(
    d: *mut SmtlDevice,
    vertex: *mut Function,
    fragment: *mut Function,
    color_formats: *const u32,
    color_count: u32,
) -> *mut RenderPipelineState {
    let d = handle!(d, std::ptr::null_mut());
    let vertex = handle!(vertex, std::ptr::null_mut());
    let mut desc = RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    if let Some(fragment) = fragment.as_mut() {
        desc.fragment = Some(StageSource::Function(fragment.function_descriptor()));
    }
    let formats: &[u32] = if color_formats.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(color_formats, color_count as usize)
    };
    for (i, &raw) in formats.iter().enumerate() {
        if raw == 0 {
            continue;
        }
        match pixel_format(raw) {
            Ok(pf) if i < desc.color_attachments.len() => {
                desc.color_attachments[i] = Some(ColorAttachmentDescriptor::new(pf));
            }
            Ok(_) => {
                set_error(format!("color attachment {i} out of range"));
                return std::ptr::null_mut();
            }
            Err(e) => {
                set_error(e);
                return std::ptr::null_mut();
            }
        }
    }
    boxed(
        "newRenderPipelineStateWithDescriptor:error:",
        d.device
            .new_render_pipeline_state(&desc, &PipelineOptions::default()),
    )
}
#[repr(C)]
pub struct SmtlColorAttachmentDesc {
    pixel_format: u32,
    blending_enabled: u32,
    source_rgb: u32,
    destination_rgb: u32,
    rgb_operation: u32,
    source_alpha: u32,
    destination_alpha: u32,
    alpha_operation: u32,
    write_mask: u32,
}

#[repr(C)]
pub struct SmtlVertexAttributeDesc {
    format: u32,
    offset: u32,
    buffer_index: u32,
}

#[repr(C)]
pub struct SmtlVertexLayoutDesc {
    stride: u32,
    step_function: u32,
    step_rate: u32,
}

#[repr(C)]
pub struct SmtlRenderPipelineDesc {
    vertex: *mut Function,
    fragment: *mut Function,
    colors: [SmtlColorAttachmentDesc; 8],
    depth_format: u32,
    stencil_format: u32,
    sample_count: u32,
    has_vertex_descriptor: u32,
    attributes: [SmtlVertexAttributeDesc; 31],
    layouts: [SmtlVertexLayoutDesc; 31],
}

/// A raw Metal enum value as its Rust type, or a message naming the field.
fn raw<T: TryFrom<u32>>(what: &str, v: u32) -> Result<T, String> {
    T::try_from(v).map_err(|_| format!("{what} {v}"))
}

fn render_descriptor(desc: &SmtlRenderPipelineDesc) -> Result<RenderPipelineDescriptor, String> {
    let vertex = unsafe { desc.vertex.as_mut() }.ok_or("no vertex function")?;
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex.function_descriptor()));
    if let Some(fragment) = unsafe { desc.fragment.as_mut() } {
        d.fragment = Some(StageSource::Function(fragment.function_descriptor()));
    }
    for (i, c) in desc.colors.iter().enumerate() {
        if c.pixel_format == 0 {
            continue;
        }
        let mut a = ColorAttachmentDescriptor::new(pixel_format(c.pixel_format)?);
        a.blending_enabled = c.blending_enabled != 0;
        a.source_rgb_blend_factor = raw("MTLBlendFactor", c.source_rgb)?;
        a.destination_rgb_blend_factor = raw("MTLBlendFactor", c.destination_rgb)?;
        a.rgb_blend_operation = raw("MTLBlendOperation", c.rgb_operation)?;
        a.source_alpha_blend_factor = raw("MTLBlendFactor", c.source_alpha)?;
        a.destination_alpha_blend_factor = raw("MTLBlendFactor", c.destination_alpha)?;
        a.alpha_blend_operation = raw("MTLBlendOperation", c.alpha_operation)?;
        a.write_mask = raw("MTLColorWriteMask", c.write_mask)?;
        d.color_attachments[i] = Some(a);
    }
    if desc.depth_format != 0 {
        d.depth_format = Some(pixel_format(desc.depth_format)?);
    }
    if desc.stencil_format != 0 {
        d.stencil_format = Some(pixel_format(desc.stencil_format)?);
    }
    d.raster_sample_count = desc.sample_count.max(1);
    if desc.has_vertex_descriptor != 0 {
        let mut v = VertexDescriptor::new();
        let mut used = [false; 31];
        for (i, a) in desc.attributes.iter().enumerate() {
            if a.format == 0 {
                continue;
            }
            if let Some(u) = used.get_mut(a.buffer_index as usize) {
                *u = true;
            }
            v.attributes.insert(
                i as u32,
                VertexAttributeDescriptor::new(raw("MTLVertexFormat", a.format)?, a.offset, a.buffer_index),
            );
        }
        for (i, l) in desc.layouts.iter().enumerate() {
            if l.stride == 0 && !used[i] {
                continue;
            }
            let mut layout = VertexBufferLayoutDescriptor::new(if l.stride == u32::MAX { None } else { Some(l.stride) });
            layout.step_function = raw("MTLVertexStepFunction", l.step_function)?;
            layout.step_rate = l.step_rate;
            v.layouts.insert(i as u32, layout);
        }
        d.vertex_descriptor = Some(v);
    }
    Ok(d)
}

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_render_pipeline_desc(
    d: *mut SmtlDevice,
    desc: *const SmtlRenderPipelineDesc,
) -> *mut RenderPipelineState {
    let d = handle!(d, std::ptr::null_mut());
    let Some(desc) = desc.as_ref() else {
        set_error("NULL descriptor".into());
        return std::ptr::null_mut();
    };
    match render_descriptor(desc) {
        Ok(rd) => boxed(
            "newRenderPipelineStateWithDescriptor:error:",
            d.device.new_render_pipeline_state(&rd, &PipelineOptions::default()),
        ),
        Err(e) => {
            set_error(e);
            std::ptr::null_mut()
        }
    }
}
release!(smtl_render_pipeline_release, RenderPipelineState);

// ---------------------------------------------------------------- queues, command buffers

#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_command_queue(d: *mut SmtlDevice) -> *mut CommandQueue {
    let d = handle!(d, std::ptr::null_mut());
    boxed("newCommandQueue", d.device.new_command_queue())
}
release!(smtl_queue_release, CommandQueue);

#[no_mangle]
pub unsafe extern "C" fn smtl_queue_command_buffer(q: *mut CommandQueue) -> *mut CommandBuffer {
    let q = handle!(q, std::ptr::null_mut());
    boxed("commandBuffer", q.command_buffer())
}
release!(smtl_cmdbuf_release, CommandBuffer);

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_commit(c: *mut CommandBuffer) -> i32 {
    let c = handle!(c, -1);
    status("commit", c.commit())
}

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_wait(c: *mut CommandBuffer, timeout_ms: u32) -> i32 {
    let c = handle!(c, -1);
    match c.wait_until_completed_for(Duration::from_millis(timeout_ms as u64)) {
        Ok(()) => match c.status() {
            CommandBufferStatus::Completed => 0,
            other => {
                set_error(format!(
                    "command buffer ended {other:?}: {}",
                    c.error_description().unwrap_or_default()
                ));
                -1
            }
        },
        Err(_) => 1,
    }
}

/// `MTLCommandBufferStatus`: NotEnqueued 0, Enqueued 1, Committed 2, Scheduled 3,
/// Completed 4, Error 5.
#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_status(c: *const CommandBuffer) -> u32 {
    match c.as_ref().map(|c| c.status()) {
        Some(CommandBufferStatus::NotEnqueued) => 0,
        Some(CommandBufferStatus::Enqueued) => 1,
        Some(CommandBufferStatus::Committed) => 2,
        Some(CommandBufferStatus::Scheduled) => 3,
        Some(CommandBufferStatus::Completed) => 4,
        _ => 5,
    }
}

// ---------------------------------------------------------------- compute

pub struct SmtlComputeEncoder(Option<ComputeCommandEncoder>);

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_compute_encoder(c: *mut CommandBuffer) -> *mut SmtlComputeEncoder {
    let c = handle!(c, std::ptr::null_mut());
    boxed(
        "computeCommandEncoder",
        c.compute_command_encoder().map(|e| SmtlComputeEncoder(Some(e))),
    )
}

macro_rules! encoder {
    ($e:expr, $what:literal) => {
        match unsafe { $e.as_mut() }.and_then(|e| e.0.as_mut()) {
            Some(e) => e,
            None => {
                set_error(format!("{}: NULL or ended encoder", $what));
                return -1;
            }
        }
    };
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_set_pipeline(
    e: *mut SmtlComputeEncoder,
    p: *mut ComputePipelineState,
) -> i32 {
    let enc = encoder!(e, "setComputePipelineState:");
    let p = handle!(p, -1);
    status("setComputePipelineState:", enc.set_compute_pipeline_state(p))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_set_buffer(
    e: *mut SmtlComputeEncoder,
    b: *mut Buffer,
    offset: u64,
    index: u32,
) -> i32 {
    let enc = encoder!(e, "setBuffer:offset:atIndex:");
    status("setBuffer:offset:atIndex:", enc.set_buffer(b.as_ref(), offset, index))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_dispatch_threads(
    e: *mut SmtlComputeEncoder,
    gx: u64,
    gy: u64,
    gz: u64,
    tx: u64,
    ty: u64,
    tz: u64,
) -> i32 {
    let enc = encoder!(e, "dispatchThreads:threadsPerThreadgroup:");
    status(
        "dispatchThreads:threadsPerThreadgroup:",
        enc.dispatch_threads(Size::new(gx, gy, gz), Size::new(tx, ty, tz)),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_compute_end(e: *mut SmtlComputeEncoder) -> i32 {
    if e.is_null() {
        return -1;
    }
    let mut boxed = Box::from_raw(e);
    match boxed.0.take() {
        Some(enc) => status("endEncoding", enc.end_encoding()),
        None => -1,
    }
}

// ---------------------------------------------------------------- render

#[repr(C)]
pub struct SmtlColorAttachment {
    pub texture: *mut Texture,
    pub load_action: u32,
    pub store_action: u32,
    pub clear: [f64; 4],
}

pub struct SmtlRenderEncoder(Option<RenderCommandEncoder>);

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_render_encoder(
    c: *mut CommandBuffer,
    colors: *const SmtlColorAttachment,
    color_count: u32,
) -> *mut SmtlRenderEncoder {
    let c = handle!(c, std::ptr::null_mut());
    let mut pass = RenderPassDescriptor::new();
    let colors: &[SmtlColorAttachment] = if colors.is_null() {
        &[]
    } else {
        std::slice::from_raw_parts(colors, color_count as usize)
    };
    for (i, a) in colors.iter().enumerate() {
        if i >= pass.color_attachments.len() {
            set_error(format!("color attachment {i} out of range"));
            return std::ptr::null_mut();
        }
        let slot = &mut pass.color_attachments[i];
        slot.texture = a.texture.as_ref().cloned();
        slot.load_action = match LoadAction::from_raw(a.load_action as u64) {
            Ok(v) => v,
            Err(e) => {
                set_error(format!("MTLLoadAction: {e:?}"));
                return std::ptr::null_mut();
            }
        };
        slot.store_action = match StoreAction::from_raw(a.store_action as u64) {
            Ok(v) => v,
            Err(e) => {
                set_error(format!("MTLStoreAction: {e:?}"));
                return std::ptr::null_mut();
            }
        };
        slot.clear_color = ClearColor::new(a.clear[0], a.clear[1], a.clear[2], a.clear[3]);
    }
    boxed(
        "renderCommandEncoderWithDescriptor:",
        c.render_command_encoder(&pass).map(|e| SmtlRenderEncoder(Some(e))),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_pipeline(
    e: *mut SmtlRenderEncoder,
    p: *mut RenderPipelineState,
) -> i32 {
    let enc = encoder!(e, "setRenderPipelineState:");
    let p = handle!(p, -1);
    status("setRenderPipelineState:", enc.set_render_pipeline_state(p))
}

/// MTLStencilDescriptor, raw Metal values.
#[repr(C)]
pub struct SmtlStencilDesc {
    compare: u32,
    stencil_failure: u32,
    depth_failure: u32,
    depth_stencil_pass: u32,
    read_mask: u32,
    write_mask: u32,
}

fn stencil(s: &SmtlStencilDesc) -> Result<StencilDescriptor, String> {
    let mut d = StencilDescriptor::default();
    d.stencil_compare_function = raw("MTLCompareFunction", s.compare)?;
    d.stencil_failure_operation = raw::<StencilOperation>("MTLStencilOperation", s.stencil_failure)?;
    d.depth_failure_operation = raw("MTLStencilOperation", s.depth_failure)?;
    d.depth_stencil_pass_operation = raw("MTLStencilOperation", s.depth_stencil_pass)?;
    d.read_mask = s.read_mask;
    d.write_mask = s.write_mask;
    Ok(d)
}

/// `newDepthStencilStateWithDescriptor:`
#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_depth_stencil_state(
    d: *mut SmtlDevice,
    depth_compare: u32,
    depth_write: i32,
    front: *const SmtlStencilDesc,
    back: *const SmtlStencilDesc,
) -> *mut DepthStencilState {
    let d = handle!(d, std::ptr::null_mut());
    let (Some(front), Some(back)) = (front.as_ref(), back.as_ref()) else {
        set_error("newDepthStencilStateWithDescriptor: NULL stencil".into());
        return std::ptr::null_mut();
    };
    let desc = (|| -> Result<DepthStencilDescriptor, String> {
        let mut desc = DepthStencilDescriptor::default();
        desc.depth_compare_function = raw::<CompareFunction>("MTLCompareFunction", depth_compare)?;
        desc.depth_write_enabled = depth_write != 0;
        desc.front_face_stencil = stencil(front)?;
        desc.back_face_stencil = stencil(back)?;
        Ok(desc)
    })();
    match desc {
        Ok(desc) => boxed("newDepthStencilStateWithDescriptor:", d.device.new_depth_stencil_state(&desc)),
        Err(e) => {
            set_error(e);
            std::ptr::null_mut()
        }
    }
}
release!(smtl_depth_stencil_state_release, DepthStencilState);

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_depth_stencil_state(
    e: *mut SmtlRenderEncoder,
    s: *mut DepthStencilState,
) -> i32 {
    let enc = encoder!(e, "setDepthStencilState:");
    status("setDepthStencilState:", enc.set_depth_stencil_state(s.as_ref()))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_stencil_reference(
    e: *mut SmtlRenderEncoder,
    front: u32,
    back: u32,
) -> i32 {
    let enc = encoder!(e, "setStencilReferenceValue:");
    enc.set_stencil_reference_values(front, back);
    0
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_vertex_buffer(
    e: *mut SmtlRenderEncoder,
    b: *mut Buffer,
    offset: u64,
    index: u32,
) -> i32 {
    let enc = encoder!(e, "setVertexBuffer:offset:atIndex:");
    status("setVertexBuffer:offset:atIndex:", enc.set_vertex_buffer(b.as_ref(), offset, index))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_fragment_buffer(
    e: *mut SmtlRenderEncoder,
    b: *mut Buffer,
    offset: u64,
    index: u32,
) -> i32 {
    let enc = encoder!(e, "setFragmentBuffer:offset:atIndex:");
    status(
        "setFragmentBuffer:offset:atIndex:",
        enc.set_fragment_buffer(b.as_ref(), offset, index),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_set_fragment_texture(
    e: *mut SmtlRenderEncoder,
    t: *mut Texture,
    index: u32,
) -> i32 {
    let enc = encoder!(e, "setFragmentTexture:atIndex:");
    status("setFragmentTexture:atIndex:", enc.set_fragment_texture(t.as_ref(), index))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_draw_primitives(
    e: *mut SmtlRenderEncoder,
    primitive_type: u32,
    start: u64,
    count: u64,
    instances: u64,
    base_instance: u64,
) -> i32 {
    let enc = encoder!(e, "drawPrimitives:");
    let prim = match PrimitiveType::try_from(primitive_type) {
        Ok(p) => p,
        Err(err) => {
            set_error(format!("MTLPrimitiveType {primitive_type}: {err:?}"));
            return -1;
        }
    };
    status(
        "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
        enc.draw_primitives(
            prim,
            start as u32,
            count as u32,
            instances as u32,
            base_instance as u32,
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_render_end(e: *mut SmtlRenderEncoder) -> i32 {
    if e.is_null() {
        return -1;
    }
    let mut boxed = Box::from_raw(e);
    match boxed.0.take() {
        Some(enc) => status("endEncoding", enc.end_encoding()),
        None => -1,
    }
}

// ---------------------------------------------------------------- blit

pub struct SmtlBlitEncoder(Option<BlitCommandEncoder>);

#[no_mangle]
pub unsafe extern "C" fn smtl_cmdbuf_blit_encoder(c: *mut CommandBuffer) -> *mut SmtlBlitEncoder {
    let c = handle!(c, std::ptr::null_mut());
    boxed(
        "blitCommandEncoder",
        c.blit_command_encoder().map(|e| SmtlBlitEncoder(Some(e))),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_copy_buffer(
    e: *mut SmtlBlitEncoder,
    src: *mut Buffer,
    src_offset: u64,
    dst: *mut Buffer,
    dst_offset: u64,
    size: u64,
) -> i32 {
    let enc = encoder!(e, "copyFromBuffer:");
    let src = handle!(src, -1);
    let dst = handle!(dst, -1);
    status(
        "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:",
        enc.copy_from_buffer_to_buffer(src, src_offset, dst, dst_offset, size),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_copy_texture_to_buffer(
    e: *mut SmtlBlitEncoder,
    src: *mut Texture,
    slice: u64,
    level: u64,
    x: u64,
    y: u64,
    w: u64,
    h: u64,
    dst: *mut Buffer,
    dst_offset: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
) -> i32 {
    let enc = encoder!(e, "copyFromTexture:");
    let src = handle!(src, -1);
    let dst = handle!(dst, -1);
    status(
        "copyFromTexture:...toBuffer:",
        enc.copy_from_texture_to_buffer(
            src,
            slice,
            level,
            Origin::new(x, y, 0),
            Size::new(w, h, 1),
            dst,
            dst_offset,
            bytes_per_row,
            bytes_per_image,
            BlitOption::None,
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_copy_texture(
    e: *mut SmtlBlitEncoder,
    src: *mut Texture,
    src_slice: u64,
    src_level: u64,
    src_origin: *const u64,         // x, y, z
    size: *const u64,               // width, height, depth
    dst: *mut Texture,
    dst_slice: u64,
    dst_level: u64,
    dst_origin: *const u64,
) -> i32 {
    let enc = encoder!(e, "copyFromTexture:toTexture:");
    let src = handle!(src, -1);
    let dst = handle!(dst, -1);
    if src_origin.is_null() || size.is_null() || dst_origin.is_null() {
        set_error("copyFromTexture:toTexture: NULL origin or size".into());
        return -1;
    }
    let so = std::slice::from_raw_parts(src_origin, 3);
    let sz = std::slice::from_raw_parts(size, 3);
    let dor = std::slice::from_raw_parts(dst_origin, 3);
    status(
        "copyFromTexture:...toTexture:",
        enc.copy_from_texture_to_texture(
            src,
            src_slice,
            src_level,
            Origin::new(so[0], so[1], so[2]),
            Size::new(sz[0], sz[1], sz[2]),
            dst,
            dst_slice,
            dst_level,
            Origin::new(dor[0], dor[1], dor[2]),
        ),
    )
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_generate_mipmaps(e: *mut SmtlBlitEncoder, t: *mut Texture) -> i32 {
    let enc = encoder!(e, "generateMipmapsForTexture:");
    let t = handle!(t, -1);
    status("generateMipmapsForTexture:", enc.generate_mipmaps(t))
}

#[no_mangle]
pub unsafe extern "C" fn smtl_blit_end(e: *mut SmtlBlitEncoder) -> i32 {
    if e.is_null() {
        return -1;
    }
    let mut boxed = Box::from_raw(e);
    match boxed.0.take() {
        Some(enc) => status("endEncoding", enc.end_encoding()),
        None => -1,
    }
}
