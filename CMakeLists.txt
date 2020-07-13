set( srcs "x_sntp_client.c" )
set( include_dirs "." )
#set( priv_include_dirs "." )
set( requires )
set( priv_requires irmacos rtos-support )
idf_component_register( SRCS ${srcs} 
						INCLUDE_DIRS ${include_dirs} 
#						PRIV_INCLUDE_DIRS ${priv_include_dirs} 
						REQUIRES ${requires} 
						PRIV_REQUIRES ${priv_requires} 
						)
