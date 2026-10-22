//! Textures over an IOSurface's pages with no copy.
//!
//! A texture the runtime makes over a no-copy buffer is a shadow: an image of its own that every
//! encoder copies the buffer's texels into and back (merged, where another writer may write the
//! bytes meanwhile). For CoreAnimation, whose every layer and framebuffer is an IOSurface, that
//! is several full-screen copies per pass and a scratch the size of the surface twice over.
//!
//! Here the surface's pages are imported once (`VK_EXT_external_memory_host`) and a linear image
//! is laid over them at the surface's own rows: an explicit `DRM_FORMAT_MOD_LINEAR` layout, which
//! turnip honours whether or not `VK_EXT_image_drm_format_modifier` is enabled. The runtime takes
//! the image as the caller's (`Device::new_texture_with_vk_image`): it renders into and samples
//! the surface's bytes themselves. Turnip's linear tiling has every feature of its optimal one
//! for color formats, and a linear image keeps no metadata, so its layout moves touch no byte.
//!
//! The image and the import belong to the caller of [`smtl_surface_image_release`], who calls it
//! only once no command buffer that used the image can still run.

use std::ffi::c_void;

use ash::vk;
use metal2vulkan::vk::ImageLayout;
use metal2vulkan_runtime::{Device, Texture, VkImageLayouts};

use crate::{boxed, set_error, texture_descriptor, SmtlDevice};

/// `MTLTextureUsage` bits.
const SHADER_READ: u32 = 1;
const SHADER_WRITE: u32 = 2;
const RENDER_TARGET: u32 = 4;

/// The surface's image, its import, and the texture over them.
pub struct SurfaceImage {
    device: Device,
    image: vk::Image,
    memory: vk::DeviceMemory,
    texture: Option<Texture>,
}

impl Drop for SurfaceImage {
    fn drop(&mut self) {
        self.texture = None;
        // SAFETY: the caller's contract: no command buffer that used the image can still run,
        // and the image and the memory are this value's own.
        unsafe {
            let raw = self.device.raw();
            if self.image != vk::Image::null() {
                raw.device.destroy_image(self.image, None);
            }
            if self.memory != vk::DeviceMemory::null() {
                raw.device.free_memory(self.memory, None);
            }
        }
    }
}

/// The Vulkan format of the Metal formats CoreAnimation renders and samples surfaces in.
fn vk_format(pixel_format_raw: u32) -> Option<vk::Format> {
    Some(match pixel_format_raw {
        10 => vk::Format::R8_UNORM,
        30 => vk::Format::R8G8_UNORM,
        70 => vk::Format::R8G8B8A8_UNORM,
        71 => vk::Format::R8G8B8A8_SRGB,
        80 => vk::Format::B8G8R8A8_UNORM,
        81 => vk::Format::B8G8R8A8_SRGB,
        90 => vk::Format::A2B10G10R10_UNORM_PACK32,
        94 => vk::Format::A2R10G10B10_UNORM_PACK32,
        115 => vk::Format::R16G16B16A16_SFLOAT,
        _ => return None,
    })
}

/// Every usage the format's linear tiling allows: the runtime asks the image for the usage its
/// own texture of the descriptor would have.
fn image_usage(features: vk::FormatFeatureFlags) -> vk::ImageUsageFlags {
    let mut usage = vk::ImageUsageFlags::empty();
    let pairs = [
        (
            vk::FormatFeatureFlags::SAMPLED_IMAGE,
            vk::ImageUsageFlags::SAMPLED,
        ),
        (
            vk::FormatFeatureFlags::STORAGE_IMAGE,
            vk::ImageUsageFlags::STORAGE,
        ),
        (
            vk::FormatFeatureFlags::COLOR_ATTACHMENT,
            vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::INPUT_ATTACHMENT,
        ),
        (
            vk::FormatFeatureFlags::TRANSFER_SRC,
            vk::ImageUsageFlags::TRANSFER_SRC,
        ),
        (
            vk::FormatFeatureFlags::TRANSFER_DST,
            vk::ImageUsageFlags::TRANSFER_DST,
        ),
    ];
    for (feature, flags) in pairs {
        if features.contains(feature) {
            usage |= flags;
        }
    }
    usage
}

/// The surface image of `size` bytes of pages at `pages` (the surface's mapping in this
/// process), its texels at `offset` with rows `bytes_per_row` apart.
#[allow(clippy::too_many_arguments)]
unsafe fn surface_image(
    device: &Device,
    pages: *mut c_void,
    size: u64,
    offset: u64,
    bytes_per_row: u64,
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    usage: u32,
) -> Result<SurfaceImage, String> {
    let format = vk_format(pixel_format_raw)
        .ok_or_else(|| format!("MTLPixelFormat {pixel_format_raw} has no surface image"))?;
    let raw = device.raw();
    let features = raw
        .instance
        .get_physical_device_format_properties(raw.physical_device, format)
        .linear_tiling_features;
    let image_usage = image_usage(features);
    let mut surface = SurfaceImage {
        device: device.clone(),
        image: vk::Image::null(),
        memory: vk::DeviceMemory::null(),
        texture: None,
    };

    let host = ash::ext::external_memory_host::Device::new(raw.instance, raw.device);
    let mut pointer = vk::MemoryHostPointerPropertiesEXT::default();
    (host.fp().get_memory_host_pointer_properties_ext)(
        raw.device.handle(),
        vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT,
        pages,
        &mut pointer,
    )
    .result()
    .map_err(|e| format!("vkGetMemoryHostPointerPropertiesEXT: {e:?}"))?;

    let plane = [vk::SubresourceLayout {
        offset,
        size: 0,
        row_pitch: bytes_per_row,
        array_pitch: 0,
        depth_pitch: 0,
    }];
    let mut explicit = vk::ImageDrmFormatModifierExplicitCreateInfoEXT::default()
        .drm_format_modifier(0) // DRM_FORMAT_MOD_LINEAR
        .plane_layouts(&plane);
    let mut external = vk::ExternalMemoryImageCreateInfo::default()
        .handle_types(vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT);
    let families = raw.queue_families;
    let mut info = vk::ImageCreateInfo::default()
        .flags(vk::ImageCreateFlags::MUTABLE_FORMAT | vk::ImageCreateFlags::EXTENDED_USAGE)
        .image_type(vk::ImageType::TYPE_2D)
        .format(format)
        .extent(vk::Extent3D {
            width: width as u32,
            height: height as u32,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .usage(image_usage)
        .initial_layout(vk::ImageLayout::PREINITIALIZED);
    if families.len() > 1 {
        info = info
            .sharing_mode(vk::SharingMode::CONCURRENT)
            .queue_family_indices(families);
    }
    let made = info.push_next(&mut explicit).push_next(&mut external);
    surface.image = raw
        .device
        .create_image(&made, None)
        .map_err(|e| format!("vkCreateImage: {e:?}"))?;

    let needs = raw.device.get_image_memory_requirements(surface.image);
    if needs.size > size {
        return Err(format!(
            "the linear image needs {} bytes, the surface has {size}",
            needs.size
        ));
    }
    let types = raw
        .instance
        .get_physical_device_memory_properties(raw.physical_device);
    let allowed = pointer.memory_type_bits & needs.memory_type_bits;
    let coherent = vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT;
    let index = (0..types.memory_type_count)
        .filter(|&i| allowed & (1 << i) != 0)
        .max_by_key(|&i| {
            types.memory_types[i as usize]
                .property_flags
                .contains(coherent)
        })
        .ok_or_else(|| {
            format!(
                "no memory type for both the pages ({:#x}) and the image ({:#x})",
                pointer.memory_type_bits, needs.memory_type_bits
            )
        })?;
    let mut import = vk::ImportMemoryHostPointerInfoEXT::default()
        .handle_type(vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT)
        .host_pointer(pages);
    let allocate = vk::MemoryAllocateInfo::default()
        .allocation_size(size)
        .memory_type_index(index)
        .push_next(&mut import);
    surface.memory = raw
        .device
        .allocate_memory(&allocate, None)
        .map_err(|e| format!("vkAllocateMemory over the surface: {e:?}"))?;
    raw.device
        .bind_image_memory(surface.image, surface.memory, 0)
        .map_err(|e| format!("vkBindImageMemory: {e:?}"))?;

    // The runtime reads the creation for what it checks the image against: a linear image of
    // turnip is what an optimal one of the same usage is, and the chain is not read.
    let stated = vk::ImageCreateInfo {
        p_next: std::ptr::null(),
        tiling: vk::ImageTiling::OPTIMAL,
        ..made
    };
    let desc = texture_descriptor(
        pixel_format_raw,
        width,
        height,
        false,
        (usage & SHADER_WRITE) | SHADER_READ | RENDER_TARGET,
        0x20, // MTLResourceStorageModePrivate: the bytes are the image's alone to the runtime
    )?;
    surface.texture = Some(
        device
            .new_texture_with_vk_image(
                &desc,
                surface.image,
                &stated,
                VkImageLayouts::new(ImageLayout::PREINITIALIZED, ImageLayout::GENERAL),
            )
            .map_err(|e| format!("new_texture_with_vk_image: {e:?}"))?,
    );
    Ok(surface)
}

/// `newTextureWithDescriptor:iosurface:plane:` with no copy: the image over the surface's
/// pages. NULL (with the reason in `smtl_last_error`) where it cannot be made; the caller then
/// falls back to a texture over a no-copy buffer.
#[no_mangle]
pub unsafe extern "C" fn smtl_device_new_surface_image(
    d: *mut SmtlDevice,
    pages: *mut c_void,
    size: u64,
    offset: u64,
    bytes_per_row: u64,
    pixel_format_raw: u32,
    width: u64,
    height: u64,
    usage: u32,
) -> *mut SurfaceImage {
    let d = handle!(d, std::ptr::null_mut());
    if pages.is_null() || size == 0 {
        set_error("smtl_device_new_surface_image: no pages".to_owned());
        return std::ptr::null_mut();
    }
    boxed(
        "smtl_device_new_surface_image",
        surface_image(
            &d.device,
            pages,
            size,
            offset,
            bytes_per_row,
            pixel_format_raw,
            width,
            height,
            usage,
        ),
    )
}

/// A handle of the surface image's texture, released with `smtl_texture_release`.
#[no_mangle]
pub unsafe extern "C" fn smtl_surface_image_texture(s: *mut SurfaceImage) -> *mut Texture {
    let s = handle!(s, std::ptr::null_mut());
    match &s.texture {
        Some(t) => Box::into_raw(Box::new(t.clone())),
        None => std::ptr::null_mut(),
    }
}

/// Destroys the image and frees the import. Only once no command buffer that used the image
/// can still run, and after every handle of its texture was released.
#[no_mangle]
pub unsafe extern "C" fn smtl_surface_image_release(s: *mut SurfaceImage) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}
