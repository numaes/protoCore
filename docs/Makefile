# Minimalist Makefile for Sphinx documentation
# This now delegates the build process to the main CMake build system.

.PHONY: html clean

# The main target to build the HTML documentation.
html:
	@echo "Building documentation via CMake..."
	@$(MAKE) -C ../build docs

# A target to clean the generated documentation.
# Note: This only cleans the Sphinx output. To clean the entire project,
# run 'rm -rf build' from the project root.
clean:
	@echo "Cleaning documentation..."
	@rm -rf _build

help:
	@echo "Please use 'make html' to build the documentation."
	@echo "The build process is now managed by CMake."
	@echo "Run 'make -C ../build docs' to build."
	@echo "Run 'make clean' to remove the generated documentation."
