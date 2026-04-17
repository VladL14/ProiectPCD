cmake_minimum_required(VERSION 3.23)

project(DockerGenDemo C)

# Căutăm pachetele furnizate de Conan
find_package(libconfig REQUIRED)
find_package(CURL REQUIRED)

# Definim executabilul pentru demo
add_executable(app app.c)

# Legăm bibliotecile de executabil folosind alias-urile generate
target_link_libraries(app PUBLIC libconfig::libconfig CURL::libcurl)