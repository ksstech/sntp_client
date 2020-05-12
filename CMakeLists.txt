idf_component_register( SRCS x_sntp_client.c 
						INCLUDE_DIRS . 
						REQUIRES
						PRIV_REQUIRES irmacos x_sockets common syslog printf hal_esp32
						)
