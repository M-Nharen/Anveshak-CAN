add_library(acc_can_loopback STATIC
  src/acc_can_loopback.c
)

target_include_directories(acc_can_loopback PUBLIC include)
target_link_libraries(acc_can_loopback PUBLIC anveshak_can_core)
