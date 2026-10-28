package org.twinsanity.app;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * Crash Twinsanity's activity: SDL3's, with the game in libmain.so (native/ANDROID.md). The disc image is picked with the
 * system's document picker (SDL's file dialog); its permission is kept across launches, so the game reads it again next time.
 */
public class TwinsanityActivity extends SDLActivity
{
    @Override
    protected String[] getLibraries()
    {
        return new String[] {"SDL3", "avutil", "avcodec", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    public void startActivityForResult(Intent intent, int requestCode)
    {
        // SDL's file dialog: the document picked stays readable in later launches (the flag the permission needs)
        if (intent != null && Intent.ACTION_OPEN_DOCUMENT.equals(intent.getAction()))
        {
            intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        }

        super.startActivityForResult(intent, requestCode);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
        // A document picked: readable in later launches too (the URI is remembered in the settings)
        if (data != null && data.getData() != null)
        {
            Uri uri = data.getData();
            try
            {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            }
            catch (SecurityException ignored)
            {
            }
        }

        super.onActivityResult(requestCode, resultCode, data);
    }
}
