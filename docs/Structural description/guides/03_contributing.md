# Guide: How to Contribute to Proto

## Introduction

Thank you for your interest in contributing to Proto! We welcome contributions from everyone, whether you're fixing a bug, improving documentation, or proposing a major new feature. This document outlines the process for contributing to ensure it's as smooth as possible for everyone involved.

## Getting Started

Before you start, it's a good idea to familiarize yourself with the project's architecture. The **[Architecture Deep Dive documents](../architecture/)** are the best place to start.

Make sure you can build the project locally by following the **[Quick Start Guide](./01_quick_start.md)**.

## The Contribution Workflow

We follow a standard GitHub-based workflow.

1.  **Fork the Repository:** Start by forking the main Proto repository to your own GitHub account.

2.  **Create a New Branch:** For each new feature or bugfix, create a descriptive branch in your fork.
    ```sh
    # Example for a new feature
    git checkout -b feature/add-new-widget

    # Example for a bugfix
    git checkout -b fix/resolve-memory-leak
    ```

3.  **Make Your Changes:** Write your code or documentation. Please adhere to the following guidelines.

4.  **Write Tests:** If you are adding a new feature or fixing a bug, please add a corresponding test case to the test suite to verify its correctness.

5.  **Ensure Tests Pass:** Run the full test suite locally to ensure your changes haven't introduced any regressions.
    ```sh
    # From the build directory
    make test
    ```

6.  **Commit Your Work:** Write clear and concise commit messages. A good commit message explains the "what" and the "why" of the change.

7.  **Push to Your Fork:** Push your branch to your personal fork on GitHub.
    ```sh
    git push origin feature/add-new-widget
    ```

8.  **Open a Pull Request (PR):** Go to the main Proto repository and open a new Pull Request. The PR description should clearly explain the changes you've made. If it resolves an existing issue, be sure to reference it (e.g., `Closes #123`).

## Coding Style

*   **C++:** We adhere to the Google C++ Style Guide. Please try to match the style of the existing codebase.
*   **Python (`proto_python`):** We follow the PEP 8 style guide.
*   **Documentation:** We use Markdown. Please keep line lengths reasonable (around 80-100 characters) to make diffs easier to read.

## Proposing Major Changes

If you want to propose a significant architectural change, please **open an issue or start a discussion** on the GitHub Discussions page first. This allows the community to discuss the proposal before you invest a lot of time in writing code. This is the best way to ensure your contribution will be aligned with the project's direction and ultimately accepted.

## Code of Conduct

We are committed to providing a welcoming and inclusive environment. All contributors are expected to abide by our Code of Conduct. (Note: A `CODE_OF_CONDUCT.md` file would need to be created for a real project).
