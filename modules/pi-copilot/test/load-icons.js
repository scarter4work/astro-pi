// Workspace process-icon fixture (fix/replay-file-params). Runs as the FIRST
// -r= script, before selftest.js, and puts real process icons on the workspace
// of this headless instance so Section J12 can read them through PCL
// (ProcessInstance::Icons / FromIcon).
//
// PJSR can read icons but cannot create them, and ImageWindow.open() on an
// .xpsm raises a modal (a hang under automation). Measured (2026-09-27): an
// .xpsm handed to the running instance with PixInsight.sh -y=SLOT FILE is
// loaded by that instance between two -r= scripts, never while a script runs.
// So this script only writes the fixture files and yields the .xpsm to its own
// instance; selftest.js (the next -r= script) finds the icons loaded.
//
// Everything lives in $PICOPILOT_SELFTEST_ICONS (a private mktemp -d of the
// harness): the fixture MARS database file the MGC icons point at, and the
// .xpsm itself. Instances are serialized by PixInsight itself (toSource), so
// the icon set is exactly what PixInsight writes. Any failure is written to
// load-error.txt there; Section J12 reports it (never a silent empty workspace).

function loadIcons( dir )
{
   // The file the MGC icons name (its content is irrelevant: only the name and
   // that it is a readable regular file matter to the fixture).
   File.writeTextFile( dir + "/MGC-icon-fixture.xmars", "PI Copilot self-test fixture, not a MARS database\n" );

   // The outer element's id attribute (the first one: a container's children
   // carry enabled="true" instead).
   function withId( src, id )
   {
      var a = src.indexOf( ' id="' );
      if ( a < 0 || a > src.indexOf( ">" ) )
         throw new Error( "no id attribute in " + src.substring( 0, 120 ) );
      var b = src.indexOf( '"', a + 5 );
      return src.substring( 0, a ) + ' id="' + id + '"' + src.substring( b + 1 );
   }

   var mgc = new MultiscaleGradientCorrection;
   mgc.useMARSDatabase = true;
   mgc.marsDatabaseFiles = [ [ true, dir + "/MGC-icon-fixture.xmars" ] ];
   mgc.grayMARSFilter = "Ha";
   mgc.gradientScale = 512;

   var pm = new PixelMath;
   pm.expression = "$T*0.9";

   var cont = new ProcessContainer;
   var mgc2 = new MultiscaleGradientCorrection;
   mgc2.useMARSDatabase = true;
   mgc2.marsDatabaseFiles = [ [ true, dir + "/MGC-icon-fixture.xmars" ] ];
   mgc2.redMARSFilter = "Ha";
   cont.add( mgc2 );
   cont.add( pm );

   // A confirmAlways process (review I1): PI Copilot must list it without opening it.
   var sa = new StarAlignment;
   sa.referenceImage = dir + "/MGC-icon-fixture.xmars";

   // A big table (a 2500-point K curve), far over one tool result: paging.
   var curves = new CurvesTransformation;
   var K = [];
   for ( var i = 0; i < 2500; ++i )
   {
      var x = i/2499;
      K.push( [ x, Math.pow( x, 0.8 ) ] );
   }
   curves.K = K;

   // Built from single-quoted pieces with no escaped quotes and no double
   // slash: the PJSR preprocessor strips comments before the parser runs and
   // mis-reads an escaped quote followed by a URL's double slash as a comment
   // start (measured: the whole script then silently never runs).
   var ns = 'http:' + '/' + '/www.pixinsight.com/xpsm';
   var xpsm = '<?xml version="1.0" encoding="UTF-8"?>\n'
            + '<xpsm version="1.0" xmlns="' + ns + '" '
            + 'xmlns:xsi="http:' + '/' + '/www.w3.org/2001/XMLSchema-instance" '
            + 'xsi:schemaLocation="' + ns + ' http:' + '/' + '/pixinsight.com/xpsm/xpsm-1.0.xsd">\n'
            + withId( mgc.toSource( 'XPSM 1.0' ), 'J12_mgc_instance' ) + '\n'
            + withId( cont.toSource( 'XPSM 1.0' ), 'J12_cont_instance' ) + '\n'
            + withId( pm.toSource( 'XPSM 1.0' ), 'J12_pm_instance' ) + '\n'
            + withId( curves.toSource( 'XPSM 1.0' ), 'J12_curves_instance' ) + '\n'
            + withId( sa.toSource( 'XPSM 1.0' ), 'J12_sa_instance' ) + '\n'
            + '<icon id="J12MGC" instance="J12_mgc_instance" xpos="8" ypos="8" workspace="Workspace01"/>\n'
            + '<icon id="J12Cont" instance="J12_cont_instance" xpos="8" ypos="56" workspace="Workspace01"/>\n'
            + '<icon id="J12PM" instance="J12_pm_instance" xpos="8" ypos="104" workspace="Workspace01"/>\n'
            + '<icon id="J12Curves" instance="J12_curves_instance" xpos="8" ypos="152" workspace="Workspace01"/>\n'
            + '<icon id="J12SA" instance="J12_sa_instance" xpos="8" ypos="200" workspace="Workspace01"/>\n'
            + '</xpsm>\n';
   File.writeTextFile( dir + "/workspace-icons.xpsm", xpsm );

   var P = new ExternalProcess;
   P.start( coreBinDirPath + "/PixInsight.sh", [ "-y=" + CoreApplication.instance, dir + "/workspace-icons.xpsm" ] );
   if ( !P.waitForFinished( 60000 ) )
   {
      P.kill();
      throw new Error( "yielding the icon set to this instance did not finish in 60 s" );
   }
   if ( P.exitCode != 0 )
      throw new Error( "PixInsight.sh -y exited " + P.exitCode + ": " + P.stdout + " " + P.stderr );
}

var PICOPILOT_ICON_DIR = getEnvironmentVariable( "PICOPILOT_SELFTEST_ICONS" );
if ( PICOPILOT_ICON_DIR.length > 0 && File.directoryExists( PICOPILOT_ICON_DIR ) )
{
   try
   {
      loadIcons( PICOPILOT_ICON_DIR );
   }
   catch ( e )
   {
      File.writeTextFile( PICOPILOT_ICON_DIR + "/load-error.txt", "load-icons.js: " + e + "\n" );
   }
}
