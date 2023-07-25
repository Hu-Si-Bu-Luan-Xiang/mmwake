set(src
"mmwake.c"
)
 
set(inc
"."
../components
)
 
 
idf_component_register(SRCS ${src}
                    INCLUDE_DIRS ${inc})
