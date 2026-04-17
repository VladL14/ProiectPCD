from conan import ConanFile
import os
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout

class DockerGenDemo(ConanFile):
    name = "DockerGenDemo"
    version = "1.0"
    author = "Lunculescu Vlad"
    settings = "os", "compiler", "build_type", "arch" 
    requires = "libconfig/1.7.3", "libcurl/8.2.1" # descarcam librariile dorite
    generators = "CMakeDeps", "CMakeToolchain"

    def layout(self): # defineste unde sa puna rezultatele din build
        cmake_layout(self)
    
    def build(self): # cmake integrat 
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
    
    def package(self):
        cmake = CMake(self)
        cmake.install()