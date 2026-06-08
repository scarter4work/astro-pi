// Sign AstroStretchStudio module
#feature-id Utilities > SignAstroStretchStudio
#feature-info Signs the AstroStretchStudio module

function main() {
   let keyFilePath = "/home/scarter4work/projects/keys/scarter4work_keys.xssk";
   let password = "Theanswertolifeis50!";
   let modulePath = "/home/scarter4work/.PixInsight/lib/x64/AstroStretchStudio-pxm.so";
   let signaturePath = "/home/scarter4work/.PixInsight/lib/x64/AstroStretchStudio-pxm.xsgn";

   console.writeln("Loading signing keys...");
   let keys = Security.loadSigningKeysFile(keyFilePath, password);

   if (!keys.valid) {
      console.criticalln("Error: Invalid keys file or password");
      return;
   }

   console.writeln("Developer ID: " + keys.developerId);
   console.writeln("Signing module: " + modulePath);

   try {
      Security.generateModuleSignatureFile(
         signaturePath,
         modulePath,
         [],  // entitlements
         keys.developerId,
         keys.publicKey,
         keys.privateKey
      );
      console.writeln("Module signed successfully!");
      console.writeln("Signature file: " + signaturePath);
   } catch (e) {
      console.criticalln("Error signing module: " + e.message);
   }

   // Secure cleanup
   keys.publicKey.secureFill();
   keys.privateKey.secureFill();
}

main();
