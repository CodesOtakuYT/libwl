project('libwl', 'c',
        version : '0.1',
        default_options : ['warning_level=2', 'c_std=c11'])

wayland_client  = dependency('wayland-client')
libdrm          = dependency('libdrm')
gbm             = dependency('gbm')
vulkan          = dependency('vulkan')
libsystemd      = dependency('libsystemd')

libwl_inc = include_directories('include', 'proto')

executable('libwl-demo',
    'demo/demo.c', 'src/libwl.c',
    'proto/xdg-shell-protocol.c',
    'proto/linux-dmabuf-unstable-v1-protocol.c',
    include_directories: libwl_inc,
    dependencies: [wayland_client, libdrm, gbm, vulkan, libsystemd],
    install: false)
