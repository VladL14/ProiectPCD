cmake_minimum_required(VERSION 3.23)

project(DockerGenDemo C)

# cautam pachetele furnizate de conan
find_package(libconfig REQUIRED)
find_package(CURL REQUIRED)
find_package(Curses REQUIRED)

# definim executabilul pentru demo
add_executable(server server.c)

# legam bibliotecile de executabil folosind aliasurile generate
target_link_libraries(server PUBLIC libconfig::libconfig CURL::libcurl)

add_executable(client client.c)

# definim executabilul pentru clientul de administrare (necesita ncurses)
add_executable(admin admin.c)
target_link_libraries(admin PUBLIC ${CURSES_LIBRARIES})