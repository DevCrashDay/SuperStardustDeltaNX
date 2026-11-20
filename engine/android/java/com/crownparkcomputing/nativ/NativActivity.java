package com.crownparkcomputing.nativ;

import org.libsdl.app.SDLActivity;

/*
 * The whole Android front end. SDL's activity does the work -- the surface, the input, the audio
 * device -- and loads the two libraries below; libmain.so is this project's runtime with the
 * recompiled game linked into it, and its SDL_main is runtime/platform/android/android_main.c,
 * which reads launch.ini for the arguments a desktop passes on the command line.
 */
public class NativActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
