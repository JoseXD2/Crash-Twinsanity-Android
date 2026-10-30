package org.twinsanity.app;

import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.WindowManager;

import java.io.File;

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
        // The game's folder on the shared storage wants "all files access" (Android 11 on): asked once, at the first start (the
        // game starts once the player is back; refused, the app's own folder stays, and the permission can be given later in the
        // system's settings)
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager())
        {
            SharedPreferences preferences = getSharedPreferences("twinsanity", MODE_PRIVATE);
            if (!preferences.getBoolean("askedAllFiles", false))
            {
                preferences.edit().putBoolean("askedAllFiles", true).apply();
                try
                {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
                }
                catch (Exception notThere)
                {
                    try
                    {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    }
                    catch (Exception ignored)
                    {
                    }
                }
            }
        }
    }

    /**
     * The game's folder on the shared storage, /sdcard/CrashTwinsanity (src/platform/native/system.cpp's SettingsFolder, through
     * JNI), made if it isn't there; null without "all files access" (then the app's own folder).
     */
    public String sharedFolder()
    {
        if (Build.VERSION.SDK_INT < 30 || !Environment.isExternalStorageManager())
        {
            return null;
        }

        File folder = new File(Environment.getExternalStorageDirectory(), "CrashTwinsanity");
        folder.mkdirs();
        return folder.isDirectory() && folder.canWrite() ? folder.getAbsolutePath() : null;
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

    /**
     * A document's name as the picker shows it, for the launcher (src/platform/native/ui/launcher.cpp, through JNI): the content://
     * URI the game keeps says nothing a player would recognise. Null when there's none.
     */
    public String displayName(String uri)
    {
        try (Cursor cursor = getContentResolver().query(Uri.parse(uri), new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null))
        {
            if (cursor != null && cursor.moveToFirst())
            {
                return cursor.getString(0);
            }
        }
        catch (Exception ignored)
        {
        }

        return null;
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
