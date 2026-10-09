/* Thin iOS shell: the game is the static libraries build_ios.sh produces, listed
 * with their frameworks in build-ios/Pikmin2.xcconfig.
 *
 * SDL's UIKit backend must own the application object, so main() hands off to
 * SDL_RunApp, which installs SDL's app delegate and then calls p2_ios_main
 * (src/ios_main.cpp). Declared here rather than included so this file needs no
 * SDL header path. */
extern int SDL_RunApp(int argc, char *argv[], int (*mainFunction)(int, char *[]), void *reserved);
extern int p2_ios_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    return SDL_RunApp(argc, argv, p2_ios_main, 0);
}
