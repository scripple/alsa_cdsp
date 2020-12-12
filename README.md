# ALSA CamillaDSP "I/O" plugin - Really an "O" plugin as only Playback (output) is supported.
This is an ALSA I/O plugin for use with CamillaDSP for audio playback.  It starts a CamillaDSP process and streams data to it via a pipe.  To playback programs it responds like a normal ALSA device.  The actual output device is whatever you configure in the CamillaDSP YAML configuration file.

To aid in handling hardware parameter changes such as sample rate, format, or the number of input channels the plugin can automatically replace fields in the YAML files with the appropriate parameters and restart CamillaDSP.

The following substitutions are available for YAML files.  hw_params are the values the playback program chooses when playing a particular audio file or stream.  The "$xxx$" format is the default behavior.  This can be overridden in the .asoundrc file to use custom tokens.

    $samplerate$ => Replaced with the sample rate set in the hw_params

    $format$ => Replaced with the format set in hw_params translated from ALSA to CamillaDSP format.

    $channels$ => Replaced with the number of channels specified in the hw_params.
    
    $extrasamples$ => Replaced with a possibly rate dependent amount of extra samples.  See below.

Here is an example CamillaDSP input YAML template.  It is the minimum configuration to create a pass through scenario.

<pre>
devices:
  samplerate: $samplerate$
  chunksize: 1024
  queuelimit: 1
  capture:
    type: File
    channels: $channels$
    filename: "/dev/stdin"
    format: $format$
  playback:
    type: ALSA
    channels: $channels$
    device: "hw:0"
    format: S32LE
</pre>

queuelimit: 1 is highly recommended or else you will experience large latency in the audio responding to user input.  It's also suggested you set the playback format to the largest bit depth your hardware can handle.  The playback device does not have to use {channels} if you are using CamillaDSP to change the number of output channels.

For more complex YAML file handling instead of specifying a config_in template you can specify a config_cmd parameter.  This command will be called with arguments "format rate channels" where format has already been converted to the CamillaDSP values.  The command should create the desired YAML file and place it in the location specified by config_out.  The command must return zero upon completion or the plugin will return an error to ALSA.

The plugin automatically enumerates as being capable of handling all the ALSA input formats CamillaDSP accepts.  The number of channels and sample rates it enumerates are specified in the ALSA .asoundrc file where this plugin is defined.

Here is a sample .asoundrc (or /etc/asound.conf) file.

<pre>
# This declares an ALSA device that you can specify to playback programs.
# You can use any name you wish that ALSA supports not just camilladsp.
# To use it you specify it to alsa like "aplay -D camilladsp"
pcm.camilladsp {
    # type cdsp must be present - it declares to use this plugin.
    # The type is NOT a variable name you can choose.
    # You can however create multiple type cdsp plugins with different names
    # if you want to specify different parameters selected by specifying a
    # different ALSA device.
    type cdsp
    
      #######################################################################
      #######################################################################
      # Required parameters.
      # The parameters in this section must be specified as a valid set.
      #######################################################################
      #######################################################################
      
      # cpath specifies the absolute path to the CamillaDSP executable.
      # CamillaDSP must have executable permission for any user that runs an
      # audio program that uses this plugin.
      cpath "/path/to/camilladsp"
      
      # config_out is the absolute path that will be passed to CamillaDSP as
      # the YAML config file argument. The file must be readable by any user
      # that runs an audio program that uses this plugin.  If the config_in
      # or config_cmd options are chosen (see below) it must also be writable
      # by those users.
      config_out "/path/to/config_out.yaml"
      
      #######################################################################
      # Parameter Passing Options
      #
      # There are three mutually exclusive ways to send the updated hw_params
      # and extra_samples to CamillaDSP.  One and only one method must be
      # specified. The three methods are config_in, config_cmd, and 
      # config_cdsp.
      #######################################################################

      # config_in is an absolute path to a YAML template the plugin will read
      # and pass along to CamillaDSP after making simple token substitutions.
      # It must be readable by any user that runs an audio program that uses
      # this plugin.
      config_in "/path/to/config_in.yaml"
      
      # config_in processing does a simple search and replace of four tokens
      # representing format, channels, sample rate, and extra samples 
      # replacing the tokens with the appropriate parameters.  The following
      # entries show the default tokens.  They can be changed to custom
      # tokens by uncommenting the lines below and replacing the default
      # tokens with the desired tokens wrapped in quotes.
      #format_token "$format$"
      #samplerate_token "$samplerate$"
      #channels_token "$channels$"
      #extrasamples_token "$extrasamples$"
      
      # config_cmd is the absolute path to a command that will be called
      # whenever the hw_params change with arguments "format rate channels".
      # It should create the YAML file CamillaDSP will load and place it in
      # the location specified by the config_out parameter.
      # Note that the extra_samples parameters specified below are NOT
      # passed in any way to this command.
      #config_cmd "/path/to/more_complex_yaml_creator"
      
      # config_cdsp says to use the new CamillaDSP internal substitutions.
      # When config_cdsp is set to an integer != 0 the hw_params and 
      # extra samples are passed to CamillaDSP on the command line as
      # -f format -r samplerate -n channels -e extra_samples
      #config_cdsp 1
      
      #######################################################################
      # End Parameter Passing Options
      #######################################################################
      
      #######################################################################
      # Capability Enumeration
      #
      # The plugin will announce the hw_params it supports based on the 
      # following settings.  Channels and sample rates must be specified.
      # The plugin will automatically enumerate all the formats that 
      # CamillaDSP supports.
      #######################################################################
      # Channels can be specified as a single value like this.
      channels 2
      # Or a range like this. (Uncomment the lines.)
      #min_channels 1
      #max_channels 2
      # But only one method can be used.
      
      # Sample rates can be configured as a specific list like this.  
      # (Up to 100 entries.)
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
      # Note that if you use a range like this ALSA will accept ANYTHING in
      # that range.  Even something odd like 45873.  If you aren't using
      # CamillaDSP's resampler and don't have a very unusual DAC you are
      # probably better off with the list format above.
      #######################################################################
      # End Capability Enumeration
      #######################################################################
      
      #######################################################################
      #######################################################################
      # End Required Parameters
      #######################################################################
      #######################################################################      

      #######################################################################
      #######################################################################
      # Optional Parameters
      #######################################################################
      ####################################################################### 
      
      # If you wish to specify additional arguments to CamillaDSP you can
      # specify them using the cargs array.  Numeric arguments must be quoted
      # in strings or the plugin will fail to load. You should not specify
      # the hw_params arguments here as the plugin will take care of that as
      # they change.
      cargs [
        -p "1234"
        -a "0.0.0.0"
        -l warn
      ]      
      
      # Extra samples can be provided in a rate scaling way for the two most
      # common audio base rates.  This is under the assumption your filters
      # will be N times as long at N times the base rate. Any of the 
      # following three values can be set or excluded in any combination.
      # If no valid extra_samples parameter is set for a given sample rate
      # the extra_samples token replacement will not occur in config_in mode
      # and the -e argument will not be issued in config_cdsp mode.
      # All extra_samples arguments must be integers >= 0.
      
      # extra_samples is used when the sample rate is not an integer multiple
      # of 44100 or 48000 or when the corresponding rate matching 
      # extra_samples option is not set.
      #
      # If using config_cdsp this is the only version of extra_samples
      # that should be specified as CamillaDSP does its own sample rate
      # based scaling of the extra samples.
      extra_samples 0
      
      # extra_samples_44100 is used when the sample rate is an integer
      # multiple of 44100. In this case {extrasamples} will be replaced with 
      # {samplerate/44100 * extra_samples_44100}.
      extra_samples_44100 8192
      
      # extra_samples_48000 is used when the sample rate is an integer
      # multiple of 48000. In this case {extrasamples} will be replaced with 
      # {samplerate/48000 * extra_samples_48000}.
      extra_samples_48000 8916
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
