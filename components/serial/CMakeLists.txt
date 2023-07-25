set(src
"serial.c"
)
 
set(inc
"include"
..
"../save/include"
)
 
 
idf_component_register(
SRCS ${src}
INCLUDE_DIRS ${inc}
REQUIRES nvs_flash iap)

