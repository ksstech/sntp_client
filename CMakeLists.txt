# SNTP CLIENT
set( srcs "client-sntp.c" )
set( include_dirs "." )
set( requires "main" )
set( priv_requires )

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
    PRIV_REQUIRES ${priv_requires}
)
