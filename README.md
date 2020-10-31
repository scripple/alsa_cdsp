# ALSA CamillaDSP "I/O" plugin - Really an "O" plugin as only Playback (output) is supported.
This is an ALSA I/O plugin for use with CamillaDSP for audio playback.  It starts a CamillaDSP process and streams data to it via a pipe.  To playback programs it responds like a normal ALSA device.  The actual output device is whatever you configure in the CamillaDSP YAML configuration file.

To aid in handling hardware parameter changes such as sample rate, format, or the number of input channels the plugin can automatically replace fields in the YAML files with the appropriate parameters and restart CamillaDSP.

The following substitutions are available for YAML files.  hw_params are the values the playback program chooses when playing a particular audio file or stream.  The "{xxx}" format is the default behavior.  This can be overridden in the .asoundrc file to use custom tokens.

    {samplerate} => Replaced with the sample rate set in the hw_params

    {format} => Replaced with the format set in hw_params translated from ALSA to CamillaDSP format.

    {channels} => Replaced with the number of channels specified in the hw_params.
    
    {extrasamples} => Replaced with a possibly rate dependent amount of extra samples.  See below.

Here is an example CamillaDSP input YAML template.  It is the minimum configuration to create a pass through scenario.

<pre>
devices:
  samplerate: {samplerate}
  chunksize: 1024
  queuelimit: 1
  capture:
    type: File
    channels: {channels}
    filename: "/dev/stdin"
    format: {format}
  playback:
    type: ALSA
    channels: {channels}
    device: "hw:0"
    format: S32LE
</pre>

queuelimit: 1 is highly recommended or else you will experience large latency in the audio responding to user input.  It's also suggested you set the playback format to the largest bit depth your hardware can handle.  The playback device does not have to use {channels} if you are using CamillaDSP to change the number of output channels.

For more complex YAML file handling instead of specifying a config_in template you can specify a config_cmd parameter.  This command will be called with arguments "format rate channels" where format has already been converted to the CamillaDSP values.  The command should create the desired YAML file and place it in the location specified by config_out.  The command must return zero upon completion or the plugin will return an error to ALSA.

The plugin automatically enumerates as being capable of handling all the ALSA input formats CamillaDSP accepts.  The number of channels and sample rates it enumerates are specified in the ALSA .asoundrc file where this plugin is defined.

Here is a sample .asoundrc (or /etc/asound.conf) file.

<pre>
pcm.camilladsp { # You can use any name, not just camilladsp
    # This must be present - it declares to use this plugin.
    # The type is NOT a variable name you can choose.
    # You can however create multiple type cdsp plugins
    # with different names (pcm.camilladsp is the name here) 
    # if you want to specify different parameters
    type cdsp
      # This is the absolute path to the CamillaDSP executable.
      # It must have executable permission for any user that
      # runs an audio program that uses this plugin.
      cpath "/path/to/camilladsp"
      
      # This is the absolute path to the YAML template the 
      # plugin will read and pass along to CamillaDSP
      # after making the above substitutions.  It must
      # be readable by any user that runs an audio program
      # that uses this plugin.
      config_in "/path/to/config_in.yaml"
      
      # An alternative to config_in is to set config_cmd.
      # This command will be called whenever the hw_params
      # change with arguments "format rate channels".  It
      # should create the YAML file CamillaDSP will load
      # and place it in the location specified by the 
      # config_out parameter.
      # config_cmd "/path/to/more_complex_yaml_creator
      
      # This is the absolute path where the plugin will
      # write the YAML file that CamillaDSP actually loads.
      # Is must be readable AND writable by any user that
      # runs an audio program that uses this plugin.
      config_out "/path/to/config_out.yaml"
      
      # If you wish to specify additional arguments to
      # CamillaDSP you can specify them using the cargs
      # array.  Numeric arguments must be quoted in strings
      # or the plugin will fail to load.
      cargs [
        -p "1234"
        -a "0.0.0.0"
        -l warn
      ]
      
      # You must specify the number of input channels this plugin
      # will enumerate to audio programs.
      # The plugin will accept a single value like this.
      channels 2
      # Or a range like this. (Uncomment the lines.)
      #min_channels 1
      #max_channels 2
      # But only one method can be used.
      
      # Sample rates that the plugin will enumerate to audio programs
      # must also be specified.  They can be configured as a specific list
      # like this.  (Up to 100 entries.)
      rates = [
        44100 
        48000 
        88200 
        96000
        176400
        192000
        352800
        384000
      ]
      # Or as a range like this.  (Uncomment the lines.)  
      #min_rate 44100
      #max_rate 384000      
      # Note that if you use a range like this ALSA will
      # accept ANYTHING in that range.  Even something 
      # odd like 45873.  If you aren't using CamillaDSP's
      # resampler and don't have a very unusual DAC you
      # are probably better off with the list format above.
      
      # Extra samples can be provided in a rate scaling way for the
      # two most common audio base rates.  This is under the assumption
      # your filters will be N times as long at N times the base rate.
      # Any of the following three values can be set or excluded in any
      # combination.
      
      # Used when the sample rate is not an integer multiple of 44100
      # or 48000 or when the corresponding rate matching extra_samples 
      # option is not set.
      extra_samples 0
      
      # Used when the sample rate is an integer multiple of 44100.
      # In this case {extrasamples} will be replaced with 
      # {samplerate/44100 * extra_samples_44100}.
      extra_samples_44100 8192

      # Used when the sample rate is an integer multiple of 48000.
      # In this case {extrasamples} will be replaced with 
      # {samplerate/48000 * extra_samples_48000}.
      extra_samples_48000 8916
      
      # The following entries allows the use of custom tokens for the
      # search and replace performed when config_in is specified.
      # To set custom tokens uncomment the lines below and replace the
      # default tokens with the desired tokens wrapped in quotes.
      #format_token "{format}"
      #rate_token "{samplerate}"
      #channels_token "{channels}"
      #ext_samp_token "{extrasamples}"
}
</pre>

If the output template is invalid for any reason CamillaDSP will fail to start.  If you get
frequent sudden failures from your audio program try checking the config_out file with
CamillaDSP's check command.  (/path/to/camilladsp -c /path/to/config_out).

Note that any changes made via the websocket will be lost whenever a new instance of CamillaDSP is launched by this plugin.  How often that happens is highly dependent on the behavior of the playback program.  If you want changes to persist you must somehow edit the template "config_in" file.

To build this plugin you need the standard build utilities (gcc) and the ALSA development package.
On a debian based system the ALSA development package is installed using the following command.

<pre>
sudo apt install libasound2-dev
</pre>

Then just build like most things.

<pre>
make
sudo make install
</pre>

Place an .asoundrc file like the one above in your home directory (for a single user) or in /etc/asound.conf for system wide definitions.

Then point your audio programs to use "camilladsp" or whatever you named your plugin declaration as the ALSA device.

Also, please note that this plugin runs CamillaDSP directly on its own.  You should not have CamillaDSP running as a service like described in the CamillaDSP documentation when using this plugin.  They might fight over the same audio output hardware.
